#include "builder/ContractBuilder.h"
#include "builder/contract/StateVarWalker.h"
#include "builder/sol-ast/stmts/SolBlock.h"
#include "builder/sol-types/TypeCoercion.h"
#include "Logger.h"

#include <libsolidity/ast/ASTVisitor.h>

namespace puyasol::builder
{

/// Collects local variable declarations inside a statement subtree (e.g. a
/// modifier body) so the inliner can rename them uniquely per application.
/// Without this, `modifier mod(uint x) { uint b = x; _; assert(b == x); }`
/// applied twice shares a single `b` slot across both instances.
class LocalVarDeclCollector: public solidity::frontend::ASTConstVisitor
{
public:
	std::vector<solidity::frontend::VariableDeclaration const*> decls;

	bool visit(solidity::frontend::VariableDeclaration const& _node) override
	{
		if (!_node.isStateVariable() && !_node.isConstant())
			decls.push_back(&_node);
		return true;
	}
};

/// Detects whether a statement subtree contains any `assembly { ... }` block.
/// Used to skip modifier local-var renaming when the body has inline assembly:
/// Yul identifiers reference their original Solidity names, and renaming would
/// produce a mismatch between the declaration slot and the assembly slot.
class InlineAssemblyDetector: public solidity::frontend::ASTConstVisitor
{
public:
	bool found = false;
	bool visit(solidity::frontend::InlineAssembly const&) override
	{
		found = true;
		return false;
	}
};

/// Conservative dead-code elimination for an inlined function body.
///
/// Modifier inlining rewrites a `return;` inside a modifier loop into an
/// unconditional `{ __mod_exit = …; break; }`. That strands the loop's
/// post-increment (`i++`) as unreachable code after the break. puya's IR
/// validator rejects unreachable code outright, whereas an IR/Yul backend
/// would silently DCE it — so puya-sol must drop it before handing the body
/// to puya (`f() m m m` in stacked_return_with_modifiers.sol).
///
/// `stmtTerminates` reports only constructs control provably cannot fall
/// through past — return / break / continue, or a block or if/else built
/// solely from those. Removing statements after such a construct is always
/// behaviour-preserving, so this is safe to run on every inlined body.
bool blockTerminates(awst::Block const* b);

bool stmtTerminates(awst::Statement const* s)
{
	if (!s) return false;
	if (dynamic_cast<awst::ReturnStatement const*>(s)) return true;
	if (dynamic_cast<awst::LoopExit const*>(s)) return true;
	if (dynamic_cast<awst::LoopContinue const*>(s)) return true;
	if (auto const* b = dynamic_cast<awst::Block const*>(s))
		return blockTerminates(b);
	if (auto const* ie = dynamic_cast<awst::IfElse const*>(s))
		return ie->elseBranch
			&& blockTerminates(ie->ifBranch.get())
			&& blockTerminates(ie->elseBranch.get());
	return false;
}

bool blockTerminates(awst::Block const* b)
{
	return b && !b->body.empty() && stmtTerminates(b->body.back().get());
}

void dropUnreachableStatements(awst::Block* b)
{
	if (!b) return;
	for (size_t i = 0; i < b->body.size(); ++i)
	{
		awst::Statement* s = b->body[i].get();
		// Recurse into nested control flow before judging this statement.
		if (auto* nb = dynamic_cast<awst::Block*>(s))
			dropUnreachableStatements(nb);
		else if (auto* wl = dynamic_cast<awst::WhileLoop*>(s))
			dropUnreachableStatements(wl->loopBody.get());
		else if (auto* ie = dynamic_cast<awst::IfElse*>(s))
		{
			dropUnreachableStatements(ie->ifBranch.get());
			dropUnreachableStatements(ie->elseBranch.get());
		}
		// Everything after an unconditional terminator is unreachable.
		if (stmtTerminates(s) && i + 1 < b->body.size())
		{
			b->body.erase(b->body.begin() + i + 1, b->body.end());
			break;
		}
	}
}

/// True for a compile-time zero / default scalar constant. Used to drop the
/// redundant `retvar = 0` the deferral would split out of an implicit
/// trailing `return 0`: the return var is already default-initialised, and
/// any earlier `return` would have left the frame, so the var is provably
/// still zero wherever a `return 0` is reached — re-assigning it there only
/// risks clobbering a value a nested `return expr;` already stored.
bool isZeroConstantExpr(awst::Expression const* val)
{
	if (auto const* i = dynamic_cast<awst::IntegerConstant const*>(val))
		return i->value == "0";
	if (auto const* b = dynamic_cast<awst::BoolConstant const*>(val))
		return !b->value;
	return false;
}

/// Free-function entry. Used by AWSTBuilder for the library /
/// internalized-lib / free-function translation path that doesn't have a
/// ContractBuilder instance. Mirrors `ContractBuilder::inlineModifiers`
/// logic but pulls all state from `_ctx` instead of `m_*` members.
///
/// Known regression vs prior session: `test_function_modifier_library` and
/// `test_function_modifier_library_inheritance` fail with "deserialization
/// failed: 'ARC4Decode'" — `s.v++` inside a modifier with storage-pointer
/// args produces an ARC4Decode assignment target that puya rejects. Prior
/// session had additional fixes (probably an ARC4Decode unwrap somewhere
/// in the assignment lowering) that were lost when the working tree was
/// reverted; full member-body parity alone isn't sufficient.
void inlineModifiers(
	FunctionTranslationCtx& _ctx,
	solidity::frontend::FunctionDefinition const& _func,
	std::shared_ptr<awst::Block>& _body
)
{
	auto& m_typeMapper = _ctx.typeMapper;
	auto& m_exprBuilder = _ctx.exprBuilder;
	auto& m_tr = _ctx.tr;
	auto const* m_currentContract = _ctx.currentContract;
	auto makeLocFree = [&](solidity::langutil::SourceLocation const& loc) {
		return makeLoc(_ctx.sourceFile, loc);
	};
	std::shared_ptr<awst::Block> __placeholder;
	auto setPlaceholderBody = [&](std::shared_ptr<awst::Block> p) {
		__placeholder = std::move(p);
	};
	auto buildBlockFree = [&](solidity::frontend::Block const& b) {
		return buildBlock(_ctx, b, __placeholder);
	};

	static int modCounter = 0;
	static int modRetvalCounter = 0;

	// Extract named return var init statements from the body and hoist them
	// BEFORE modifier arg evaluation.
	std::set<std::string> returnParamNames;
	for (auto const& rp : _func.returnParameters())
		if (!rp->name().empty())
			returnParamNames.insert(rp->name());

	// Unnamed returns: synthesise return vars so `return expr;` can be
	// rewritten into `__mod_retval_N = expr;` (in placeholder) + deferred
	// `return __mod_retval_N;`.
	std::vector<std::pair<std::string, awst::WType const*>> syntheticRets;
	bool allUnnamed = !_func.returnParameters().empty();
	for (auto const& rp: _func.returnParameters())
		if (!rp->name().empty()) { allUnnamed = false; break; }
	if (returnParamNames.empty() && allUnnamed)
	{
		int baseId = modRetvalCounter++;
		for (size_t i = 0; i < _func.returnParameters().size(); ++i)
		{
			auto* t = m_typeMapper.map(_func.returnParameters()[i]->type());
			std::string n = "__mod_retval_" + std::to_string(baseId)
				+ (_func.returnParameters().size() > 1 ? "_" + std::to_string(i) : "");
			syntheticRets.emplace_back(n, t);
			returnParamNames.insert(n);
		}
	}

	std::vector<std::shared_ptr<awst::Statement>> hoistedInits;
	if (!returnParamNames.empty() && !_body->body.empty())
	{
		std::set<std::string> seen;
		auto it = _body->body.begin();
		while (it != _body->body.end())
		{
			if (auto* assign = dynamic_cast<awst::AssignmentStatement*>(it->get()))
			{
				auto* target = dynamic_cast<awst::VarExpression*>(assign->target.get());
				bool isZeroInit = false;
				auto const* val = assign->value.get();
				if (auto* intConst = dynamic_cast<awst::IntegerConstant const*>(val))
					isZeroInit = (intConst->value == "0");
				else if (auto* boolConst = dynamic_cast<awst::BoolConstant const*>(val))
					isZeroInit = !boolConst->value;
				else if (dynamic_cast<awst::BytesConstant const*>(val))
					isZeroInit = true;
				else if (dynamic_cast<awst::NewStruct const*>(val)
					|| dynamic_cast<awst::NewArray const*>(val)
					|| dynamic_cast<awst::TupleExpression const*>(val))
					isZeroInit = true;

				if (target && returnParamNames.count(target->name) && isZeroInit
					&& !seen.count(target->name))
				{
					seen.insert(target->name);
					hoistedInits.push_back(std::move(*it));
					it = _body->body.erase(it);
					continue;
				}
				if (target && !returnParamNames.count(target->name))
				{
					++it;
					continue;
				}
				break;
			}
			if (dynamic_cast<awst::ExpressionStatement*>(it->get()))
			{
				++it;
				continue;
			}
			break;
		}
	}
	for (auto const& [n, t]: syntheticRets)
	{
		auto loc = makeLocFree(_func.location());
		hoistedInits.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(n, t, loc),
			StorageMapper::makeDefaultValue(t, loc),
			loc));
	}

	for (auto const& modInvocation: _func.modifiers())
	{
		auto const* modDef = dynamic_cast<solidity::frontend::ModifierDefinition const*>(
			modInvocation->name().annotation().referencedDeclaration);

		if (!modDef)
			continue;

		{
			bool isExplicit = modInvocation->name().path().size() > 1;
			if (m_currentContract && !isExplicit)
			{
				std::string modName = modDef->name();
				solidity::frontend::ModifierDefinition const* resolved = nullptr;
				forEachFunctionModifier(*m_currentContract, [&](auto const* mod)
				{
					if (resolved) return;
					if (mod->name() == modName) resolved = mod;
				});
				if (resolved) modDef = resolved;
			}
		}

		auto modBody = awst::makeBlock(makeLocFree(modDef->location()));

		if (modDef->body().statements().empty())
			continue;

		auto const* args = modInvocation->arguments();
		auto const& params = modDef->parameters();
		std::vector<int64_t> remappedDeclIds;

		if (args && !args->empty())
		{
			auto modLoc = makeLocFree(modInvocation->location());
			for (size_t i = 0; i < args->size() && i < params.size(); ++i)
			{
				auto const& param = params[i];

				auto argExpr = m_exprBuilder.build(*(*args)[i]);
				if (!argExpr)
					continue;

				// Storage-pointer params (`S storage s`, `mapping(K=>V) storage m`):
				// don't materialise a local copy. The arg expression itself is the
				// storage location (BoxValueExpression, FieldExpression on a
				// state-var-backed struct, etc.) and writes inside the modifier
				// body must mutate the underlying storage, not a local copy.
				// Set the storage alias so SolIdentifier resolves the param to
				// the original storage expression.
				if (param->referenceLocation()
					== solidity::frontend::VariableDeclaration::Location::Storage)
				{
					// Modifier args come from the caller's expression
					// context; pick the alias kind from the AWST shape.
					sol_ast::StorageAlias alias = [&]() -> sol_ast::StorageAlias {
						if (dynamic_cast<awst::BytesConstant const*>(argExpr.get()))
							return sol_ast::StorageAlias::mappingHolder(std::move(argExpr));
						if (dynamic_cast<awst::IndexExpression const*>(argExpr.get()))
							return sol_ast::StorageAlias::indexedPath(std::move(argExpr));
						if (dynamic_cast<awst::FieldExpression const*>(argExpr.get()))
							return sol_ast::StorageAlias::fieldPath(std::move(argExpr));
						if (dynamic_cast<awst::TupleItemExpression const*>(argExpr.get()))
							return sol_ast::StorageAlias::tupleSlice(std::move(argExpr));
						return sol_ast::StorageAlias::stateRead(std::move(argExpr));
					}();
					m_tr.setStorageAlias(param->id(), std::move(alias));
					// Track for cleanup via the same remappedDeclIds list, so
					// the post-body sweep removes the alias too.
					remappedDeclIds.push_back(param->id());
					continue;
				}

				std::string uniqueName = "__mod_" + param->name() + "_" + std::to_string(modCounter++);
				auto* paramType = m_typeMapper.map(param->type());

				argExpr = TypeCoercion::implicitNumericCast(
					std::move(argExpr), paramType, modLoc);

				auto target = awst::makeVarExpression(uniqueName, paramType, modLoc);

				auto assignment = awst::makeAssignmentStatement(target, std::move(argExpr), modLoc);
				modBody->body.push_back(std::move(assignment));

				m_tr.setParamRemap(param->id(), sol_ast::ParamRemap{uniqueName, paramType});
				remappedDeclIds.push_back(param->id());
			}
		}

		{
			InlineAssemblyDetector asmDetector;
			modDef->body().accept(asmDetector);
			if (!asmDetector.found)
			{
				LocalVarDeclCollector localCollector;
				modDef->body().accept(localCollector);
				for (auto const* localDecl: localCollector.decls)
				{
					std::string uniqueName
						= "__mod_local_" + localDecl->name() + "_" + std::to_string(modCounter++);
					auto* localType = m_typeMapper.map(localDecl->type());
					m_tr.setParamRemap(localDecl->id(), sol_ast::ParamRemap{uniqueName, localType});
					remappedDeclIds.push_back(localDecl->id());
				}
			}
		}

		auto placeholderBody = awst::makeBlock(_body->sourceLocation);
		std::shared_ptr<awst::Statement> deferredReturn;
		std::set<std::string> hoistedReturnVars;

		for (auto const& bodyStmt: _body->body)
		{
			if (auto const* retStmt = dynamic_cast<awst::ReturnStatement const*>(bodyStmt.get()))
			{
				if (returnParamNames.size() == 1 && retStmt->value)
				{
					auto const& retName = *returnParamNames.begin();
					bool isJustRetVar = false;
					if (auto const* varRef = dynamic_cast<awst::VarExpression const*>(retStmt->value.get()))
						isJustRetVar = (varRef->name == retName);

					// Skip a redundant `retvar = 0` split from an implicit
					// trailing `return 0`: the synthetic return var is already
					// 0-initialised and is only ever written by return-handling,
					// so a value-carrying `return` nested in a loop would
					// otherwise be clobbered here. Restricted to synthetic
					// `__mod_retval_*` vars — a *named* return var can be
					// assigned directly by user code, so its `return 0` must
					// still emit the assignment.
					if (!isJustRetVar
						&& !(isZeroConstantExpr(retStmt->value.get())
							&& retName.rfind("__mod_retval_", 0) == 0))
					{
						auto target = awst::makeVarExpression(retName, retStmt->value->wtype, retStmt->sourceLocation);
						placeholderBody->body.push_back(
							awst::makeAssignmentStatement(std::move(target), retStmt->value, retStmt->sourceLocation));
					}

					auto retVar = awst::makeVarExpression(retName, retStmt->value->wtype, retStmt->sourceLocation);
					deferredReturn = awst::makeReturnStatement(std::move(retVar), retStmt->sourceLocation);
				}
				else if (syntheticRets.size() > 1 && retStmt->value)
				{
					auto const* tupleVal = dynamic_cast<awst::TupleExpression const*>(retStmt->value.get());
					if (tupleVal && tupleVal->items.size() == syntheticRets.size())
					{
						for (size_t i = 0; i < syntheticRets.size(); ++i)
						{
							auto target = awst::makeVarExpression(
								syntheticRets[i].first, syntheticRets[i].second, retStmt->sourceLocation);
							placeholderBody->body.push_back(awst::makeAssignmentStatement(
								std::move(target), tupleVal->items[i], retStmt->sourceLocation));
						}
					}
					else
					{
						auto tupleTarget = awst::makeTupleExpression(nullptr, retStmt->sourceLocation);
						std::vector<awst::WType const*> tupleTypes;
						for (auto const& [n, t]: syntheticRets)
						{
							tupleTarget->items.push_back(
								awst::makeVarExpression(n, t, retStmt->sourceLocation));
							tupleTypes.push_back(t);
						}
						tupleTarget->wtype = m_typeMapper.createType<awst::WTuple>(
							std::move(tupleTypes), std::nullopt);
						placeholderBody->body.push_back(awst::makeAssignmentStatement(
							std::move(tupleTarget), retStmt->value, retStmt->sourceLocation));
					}

					auto deferTuple = awst::makeTupleExpression(nullptr, retStmt->sourceLocation);
					std::vector<awst::WType const*> tupleTypes;
					for (auto const& [n, t]: syntheticRets)
					{
						deferTuple->items.push_back(
							awst::makeVarExpression(n, t, retStmt->sourceLocation));
						tupleTypes.push_back(t);
					}
					deferTuple->wtype = m_typeMapper.createType<awst::WTuple>(
						std::move(tupleTypes), std::nullopt);
					deferredReturn = awst::makeReturnStatement(
						std::move(deferTuple), retStmt->sourceLocation);
				}
				else
					deferredReturn = bodyStmt;
			}
			else if (!returnParamNames.empty())
			{
				auto const* assign = dynamic_cast<awst::AssignmentStatement const*>(bodyStmt.get());
				auto const* targetVar = assign
					? dynamic_cast<awst::VarExpression const*>(assign->target.get())
					: nullptr;
				if (targetVar && returnParamNames.count(targetVar->name)
					&& !hoistedReturnVars.count(targetVar->name))
				{
					modBody->body.push_back(bodyStmt);
					hoistedReturnVars.insert(targetVar->name);
				}
				else
					placeholderBody->body.push_back(bodyStmt);
			}
			else
				placeholderBody->body.push_back(bodyStmt);
		}

		setPlaceholderBody(placeholderBody);
		auto translatedModBody = buildBlockFree(modDef->body());
		setPlaceholderBody(nullptr);

		if (translatedModBody)
		{
			static int modExitCounter = 0;
			std::string flagName = "__mod_exit_" + std::to_string(modExitCounter++);
			auto flagLoc = translatedModBody->sourceLocation;

			auto makeFlagSet = [&]() -> std::shared_ptr<awst::Statement> {
				auto target = awst::makeVarExpression(flagName, awst::WType::boolType(), flagLoc);
				return awst::makeAssignmentStatement(std::move(target), awst::makeTrue(flagLoc), flagLoc);
			};
			auto makeBreak = [&]() -> std::shared_ptr<awst::Statement> {
				return awst::makeLoopExit(flagLoc);
			};
			auto makeFlagCheck = [&]() -> std::shared_ptr<awst::Statement> {
				auto cond = awst::makeVarExpression(flagName, awst::WType::boolType(), flagLoc);
				auto branchBody = awst::makeBlock(flagLoc);
				branchBody->body.push_back(makeBreak());
				return awst::makeIfElse(std::move(cond), std::move(branchBody), nullptr, flagLoc);
			};

			bool hasReturnInLoop = false;
			std::function<void(std::vector<std::shared_ptr<awst::Statement>>&, bool)> replaceReturns;
			replaceReturns = [&](std::vector<std::shared_ptr<awst::Statement>>& stmts, bool inLoop) {
				for (size_t i = 0; i < stmts.size(); ++i)
				{
					auto& s = stmts[i];
					if (auto* retSt = dynamic_cast<awst::ReturnStatement*>(s.get()))
					{
						auto block = awst::makeBlock(s->sourceLocation);
						// A valued return nested in a loop/branch is the inlined
						// inner body's `return expr;` — preserve the value by
						// assigning it to the single return var before the
						// modifier exit (top-level returns are handled by the
						// deferral pass). Bare `return;` → flag+break only.
						if (retSt->value && returnParamNames.size() == 1)
						{
							auto const& retName = *returnParamNames.begin();
							bool isJustRetVar = false;
							if (auto const* vr = dynamic_cast<awst::VarExpression const*>(
									retSt->value.get()))
								isJustRetVar = (vr->name == retName);
							if (!isJustRetVar)
							{
								auto target = awst::makeVarExpression(
									retName, retSt->value->wtype, retSt->sourceLocation);
								block->body.push_back(awst::makeAssignmentStatement(
									std::move(target), retSt->value, retSt->sourceLocation));
							}
						}
						block->body.push_back(makeFlagSet());
						block->body.push_back(makeBreak());
						s = std::move(block);
						if (inLoop) hasReturnInLoop = true;
					}
					else if (auto* ifElse = dynamic_cast<awst::IfElse*>(s.get()))
					{
						if (ifElse->ifBranch) replaceReturns(ifElse->ifBranch->body, inLoop);
						if (ifElse->elseBranch) replaceReturns(ifElse->elseBranch->body, inLoop);
					}
					else if (auto* block = dynamic_cast<awst::Block*>(s.get()))
						replaceReturns(block->body, inLoop);
					else if (auto* whileLoop = dynamic_cast<awst::WhileLoop*>(s.get()))
					{
						if (whileLoop->loopBody)
							replaceReturns(whileLoop->loopBody->body, /*inLoop=*/true);
					}
				}
			};
			replaceReturns(translatedModBody->body, false);

			if (hasReturnInLoop)
			{
				std::function<void(std::vector<std::shared_ptr<awst::Statement>>&)> insertFlagChecks;
				insertFlagChecks = [&](std::vector<std::shared_ptr<awst::Statement>>& stmts) {
					for (size_t i = 0; i < stmts.size(); ++i)
					{
						if (dynamic_cast<awst::WhileLoop*>(stmts[i].get()))
						{
							stmts.insert(stmts.begin() + i + 1, makeFlagCheck());
							++i;
						}
						else if (auto* ifElse = dynamic_cast<awst::IfElse*>(stmts[i].get()))
						{
							if (ifElse->ifBranch) insertFlagChecks(ifElse->ifBranch->body);
							if (ifElse->elseBranch) insertFlagChecks(ifElse->elseBranch->body);
						}
						else if (auto* block = dynamic_cast<awst::Block*>(stmts[i].get()))
							insertFlagChecks(block->body);
					}
				};
				insertFlagChecks(translatedModBody->body);
			}

			modBody->body.push_back(awst::makeAssignmentStatement(
				awst::makeVarExpression(flagName, awst::WType::boolType(), flagLoc),
				awst::makeFalse(flagLoc),
				flagLoc));

			auto loopBody = awst::makeBlock(flagLoc);
			for (auto& stmt: translatedModBody->body)
				loopBody->body.push_back(std::move(stmt));
			loopBody->body.push_back(makeBreak());

			modBody->body.push_back(awst::makeWhileLoop(
				awst::makeTrue(flagLoc), std::move(loopBody), flagLoc));
		}
		else
		{
			for (auto const& bodyStmt: _body->body)
				modBody->body.push_back(bodyStmt);
		}

		if (deferredReturn)
			modBody->body.push_back(std::move(deferredReturn));

		for (auto declId: remappedDeclIds)
			m_tr.eraseParamRemap(declId);

		_body = modBody;
	}

	if (!hoistedInits.empty())
	{
		_body->body.insert(
			_body->body.begin(),
			std::make_move_iterator(hoistedInits.begin()),
			std::make_move_iterator(hoistedInits.end()));
	}

	dropUnreachableStatements(_body.get());
}

void ContractBuilder::inlineModifiers(
	solidity::frontend::FunctionDefinition const& _func,
	std::shared_ptr<awst::Block>& _body
)
{
	// Contract methods and constructors share the free-function modifier
	// inliner above. `makeFunctionCtx()` packages this builder's
	// per-function state into the FunctionTranslationCtx the free
	// `inlineModifiers` expects — the same wrapper `ContractBuilder::
	// buildBlock` already routes through. (This routine was previously a
	// ~490-line copy that had drifted out of sync with the free version,
	// most notably lacking its storage-pointer modifier-param handling.)
	auto ctx = makeFunctionCtx();
	::puyasol::builder::inlineModifiers(ctx, _func, _body);
}

void ContractBuilder::buildModifierChain(
	solidity::frontend::FunctionDefinition const& _func,
	awst::ContractMethod& _method,
	std::string const& _contractName
)
{
	static int modChainCounter = 0;
	int chainId = modChainCounter++;

	auto const& modifiers = _func.modifiers();
	if (modifiers.empty())
		return;

	std::string baseName = _func.name();
	std::string cref = m_sourceFile + "." + _contractName;

	// Step 1: Create the innermost subroutine (function body without modifiers)
	std::string bodySubName = baseName + "__body_" + std::to_string(chainId);
	{
		awst::ContractMethod bodySub;
		bodySub.sourceLocation = _method.sourceLocation;
		bodySub.cref = cref;
		bodySub.memberName = bodySubName;
		bodySub.returnType = _method.returnType;
		bodySub.args = _method.args; // same params as outer function
		bodySub.body = _method.body; // move the original function body here
		bodySub.arc4MethodConfig = std::nullopt; // internal, not ABI-routable
		bodySub.pure = _method.pure;
		m_modifierSubroutines.push_back(std::move(bodySub));
	}

	// Step 2: Build modifier subroutines from innermost to outermost.
	// Each calls the next (or the body sub for the innermost modifier).
	std::string nextSubName = bodySubName;

	for (int i = static_cast<int>(modifiers.size()) - 1; i >= 0; --i)
	{
		auto const& modInvocation = modifiers[i];
		auto const* modDef = dynamic_cast<solidity::frontend::ModifierDefinition const*>(
			modInvocation->name().annotation().referencedDeclaration
		);
		if (!modDef)
		{
			// Constructor base call — skip, handled elsewhere
			continue;
		}

		// Resolve virtual overrides — but NOT for explicit base modifier calls (A.m).
		// Detect explicit base: the IdentifierPath has >1 component for A.m.
		// For inherited functions, the referencedDeclaration points to the base
		// modifier, but we still want the most-derived override.
		bool isExplicitBaseModifier = false;
		{
			// Check the IdentifierPath: "A.m" has path ["A", "m"], "m" has path ["m"]
			auto const& path = modInvocation->name().path();
			if (path.size() > 1)
				isExplicitBaseModifier = true;
		}

		if (m_currentContract && !isExplicitBaseModifier)
		{
			std::string modName = modDef->name();
			solidity::frontend::ModifierDefinition const* resolved = nullptr;
			forEachFunctionModifier(*m_currentContract, [&](auto const* mod)
			{
				if (resolved) return;
				if (mod->name() == modName) resolved = mod;
			});
			if (resolved) modDef = resolved;
		}

		std::string modSubName = baseName + "__mod" + std::to_string(i) + "_" + std::to_string(chainId);

		// Create the modifier subroutine
		awst::ContractMethod modSub;
		modSub.sourceLocation = makeLoc(modDef->location());
		modSub.cref = cref;
		modSub.memberName = modSubName;
		modSub.returnType = _method.returnType;
		modSub.args = _method.args; // receives same params
		modSub.arc4MethodConfig = std::nullopt; // internal
		modSub.pure = _method.pure;

		// Build modifier body block
		auto modBody = awst::makeBlock(modSub.sourceLocation);

		// Evaluate modifier arguments → bind to local vars via paramRemaps
		auto const* args = modInvocation->arguments();
		auto const& params = modDef->parameters();
		std::vector<int64_t> remappedDeclIds;
		static int modArgCounter = 0;

		if (args && !args->empty())
		{
			auto modLoc = makeLoc(modInvocation->location());
			for (size_t pi = 0; pi < args->size() && pi < params.size(); ++pi)
			{
				auto const& param = params[pi];
				std::string uniqueName = "__mod_" + param->name() + "_" + std::to_string(modArgCounter++);
				auto* paramType = m_typeMapper.map(param->type());

				auto argExpr = m_exprBuilder->build(*(*args)[pi]);
				if (!argExpr) continue;
				argExpr = TypeCoercion::implicitNumericCast(std::move(argExpr), paramType, modLoc);

				auto target = awst::makeVarExpression(uniqueName, paramType, modLoc);

				auto assignment = awst::makeAssignmentStatement(target, std::move(argExpr), modLoc);
				modBody->body.push_back(std::move(assignment));

				m_tr->setParamRemap(param->id(), sol_ast::ParamRemap{uniqueName, paramType});
				remappedDeclIds.push_back(param->id());
			}
		}

		// Set placeholder body: at `_;`, call the next subroutine
		// Build a block that calls nextSubName and assigns return value
		auto placeholderBlock = awst::makeBlock(modSub.sourceLocation);

		// Determine return variable name for this modifier sub
		std::string retVarName;
		auto const& retParams = _func.returnParameters();
		if (_method.returnType != awst::WType::voidType())
		{
			if (retParams.size() == 1 && !retParams[0]->name().empty())
				retVarName = retParams[0]->name();
			else
				retVarName = "__retval_" + std::to_string(chainId) + "_" + std::to_string(i);
		}

		{
			// Build: returnVar = nextSub(args...)
			auto call = awst::makeSubroutineCall(awst::InstanceMethodTarget{nextSubName}, _method.returnType, modSub.sourceLocation);
			for (auto const& arg: _method.args)
				awst::pushCallArg(call->args, arg.name,
					awst::makeVarExpression(arg.name, arg.wtype, modSub.sourceLocation));

			if (!retVarName.empty())
			{
				// Assign call result to return variable
				auto retTarget = awst::makeVarExpression(retVarName, _method.returnType, modSub.sourceLocation);

				auto assign = awst::makeAssignmentStatement(std::move(retTarget), std::move(call), modSub.sourceLocation);
				placeholderBlock->body.push_back(std::move(assign));
			}
			else
			{
				auto stmt = awst::makeExpressionStatement(std::move(call), modSub.sourceLocation);
				placeholderBlock->body.push_back(std::move(stmt));
			}
		}

		// Initialize return variable at the start of the modifier sub
		if (!retVarName.empty())
		{
			auto target = awst::makeVarExpression(retVarName, _method.returnType, modSub.sourceLocation);
			auto zeroVal = StorageMapper::makeDefaultValue(_method.returnType, modSub.sourceLocation);
			auto assign = awst::makeAssignmentStatement(std::move(target), std::move(zeroVal), modSub.sourceLocation);
			modBody->body.push_back(std::move(assign));
		}

		// Translate modifier body with _;→placeholderBlock
		setPlaceholderBody(placeholderBlock);
		auto translatedBody = buildBlock(modDef->body());
		setPlaceholderBody(nullptr);

		if (translatedBody)
		{
			// Fix bare return statements in modifier body: `return;` → `return __retval;`
			// A modifier's bare `return` means "exit early", returning the current retval.
			if (!retVarName.empty())
			{
				std::function<void(std::vector<std::shared_ptr<awst::Statement>>&)> fixReturns;
				fixReturns = [&](std::vector<std::shared_ptr<awst::Statement>>& stmts)
				{
					for (auto& stmt: stmts)
					{
						if (auto* ret = dynamic_cast<awst::ReturnStatement*>(stmt.get()))
						{
							if (!ret->value)
							{
								auto var = awst::makeVarExpression(retVarName, _method.returnType, ret->sourceLocation);
								ret->value = std::move(var);
							}
						}
						if (auto* ifElse = dynamic_cast<awst::IfElse*>(stmt.get()))
						{
							if (ifElse->ifBranch) fixReturns(ifElse->ifBranch->body);
							if (ifElse->elseBranch) fixReturns(ifElse->elseBranch->body);
						}
						if (auto* block = dynamic_cast<awst::Block*>(stmt.get()))
							fixReturns(block->body);
						if (auto* loop = dynamic_cast<awst::WhileLoop*>(stmt.get()))
							if (loop->loopBody) fixReturns(loop->loopBody->body);
					}
				};
				fixReturns(translatedBody->body);
			}

			for (auto& stmt: translatedBody->body)
				modBody->body.push_back(std::move(stmt));
		}

		// Add return statement using the return variable
		{
			auto retStmt = awst::makeReturnStatement(nullptr, modSub.sourceLocation);
			if (!retVarName.empty())
			{
				auto var = awst::makeVarExpression(retVarName, _method.returnType, modSub.sourceLocation);
				retStmt->value = std::move(var);
			}
			modBody->body.push_back(std::move(retStmt));
		}

		// Unregister remaps
		for (auto declId: remappedDeclIds)
			m_tr->eraseParamRemap(declId);

		modSub.body = modBody;
		m_modifierSubroutines.push_back(std::move(modSub));
		nextSubName = modSubName;
	}

	// Step 3: Replace _method.body with a call to the outermost modifier subroutine
	auto entryBody = awst::makeBlock(_method.sourceLocation);

	// Initialize named return vars to zero
	for (auto const& rp: _func.returnParameters())
	{
		if (rp->name().empty()) continue;
		auto* rpType = m_typeMapper.map(rp->type());
		auto target = awst::makeVarExpression(rp->name(), rpType, _method.sourceLocation);

		auto zeroVal = StorageMapper::makeDefaultValue(rpType, _method.sourceLocation);
		auto assign = awst::makeAssignmentStatement(std::move(target), std::move(zeroVal), _method.sourceLocation);
		entryBody->body.push_back(std::move(assign));
	}

	// Call outermost modifier sub
	auto call = awst::makeSubroutineCall(awst::InstanceMethodTarget{nextSubName}, _method.returnType, _method.sourceLocation);
	for (auto const& arg: _method.args)
		awst::pushCallArg(call->args, arg.name,
			awst::makeVarExpression(arg.name, arg.wtype, _method.sourceLocation));

	if (_method.returnType != awst::WType::voidType())
	{
		auto retStmt = awst::makeReturnStatement(std::move(call), _method.sourceLocation);
		entryBody->body.push_back(std::move(retStmt));
	}
	else
	{
		auto stmt = awst::makeExpressionStatement(std::move(call), _method.sourceLocation);
		entryBody->body.push_back(std::move(stmt));
	}

	_method.body = entryBody;
}

} // namespace puyasol::builder
