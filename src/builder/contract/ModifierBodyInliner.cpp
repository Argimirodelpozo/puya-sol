/// @file ModifierBodyInliner.cpp
/// Textual `_` expansion for modifier bodies. Used for CONSTRUCTORS and
/// library / free functions (bodies inlined in place, no callable method).
/// Contract METHODS use the solc-aligned subroutine chain instead — the
/// default since the modifier-lowering consolidation — in
/// ModifierInliner.cpp::buildModifierChain.

#include "builder/contract/ContractBuilder.h"
#include "awst/StatementWalk.h"
#include "awst/NameGen.h"
#include "builder/contract/StateVarWalker.h"
#include "builder/sol-ast/stmts/SolBlock.h"
#include "builder/sol-types/TypeCoercion.h"

#include <libsolidity/ast/ASTVisitor.h>

namespace puyasol::builder
{

namespace {

/// Collect local vars in a modifier body so the inliner can rename them
/// uniquely per application (prevents slot collision across stacked modifiers).
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

/// Detect inline assembly: renaming local vars would mismatch Yul identifier refs.
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

/// Drop unreachable statements after inlining. Modifier inlining rewrites
/// `return;` in a loop to `{ __mod_exit=…; break; }`, stranding the post-
/// increment as unreachable. puya IR rejects unreachable code; EVM backends
/// silently DCE it. Handles stacked_return_with_modifiers.sol (f() m m m).
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
		// Recurse into nested control flow before judging this statement
		// (awst::forEachChildBlock — the single container enumeration, T5).
		awst::forEachChildBlock(*s, [&](awst::Block& b, bool) {
			dropUnreachableStatements(&b);
		});
		// Everything after an unconditional terminator is unreachable.
		if (stmtTerminates(s) && i + 1 < b->body.size())
		{
			b->body.erase(b->body.begin() + i + 1, b->body.end());
			break;
		}
	}
}

/// True for a compile-time zero/default constant. Used to suppress the redundant
/// `retvar = 0` split from an implicit trailing `return 0`; the var is already
/// default-initialised and re-assigning risks clobbering a nested `return expr`.
bool isZeroConstantExpr(awst::Expression const* val)
{
	if (auto const* i = dynamic_cast<awst::IntegerConstant const*>(val))
		return i->value == "0";
	if (auto const* b = dynamic_cast<awst::BoolConstant const*>(val))
		return !b->value;
	return false;
}

} // namespace

/// Free-function/library entry point (no ContractBuilder instance).
/// Known regression: test_function_modifier_library / _inheritance fail with
/// "deserialization failed: 'ARC4Decode'" — `s.v++` with storage-pointer
/// modifier args produces an ARC4Decode assignment target that puya rejects.
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


	// Hoist named-return zero-inits before modifier arg evaluation.
	std::set<std::string> returnParamNames;
	for (auto const& rp : _func.returnParameters())
		if (!rp->name().empty())
			returnParamNames.insert(rp->name());

	// Unnamed returns: synthesise __mod_retval_N vars so `return expr;` can be
	// split into `__mod_retval_N = expr;` (placeholder) + deferred `return __mod_retval_N;`.
	std::vector<std::pair<std::string, awst::WType const*>> syntheticRets;
	bool allUnnamed = !_func.returnParameters().empty();
	for (auto const& rp: _func.returnParameters())
		if (!rp->name().empty()) { allUnnamed = false; break; }
	if (returnParamNames.empty() && allUnnamed)
	{
		int baseId = awst::NameGen::next("ModifierBodyInliner.modRetvalCounter");
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

	// Iterate modifiers RIGHT-to-LEFT: each pass wraps the accumulated _body as the new
	// modifier's placeholder (i.e. as its inner layer), so processing the rightmost first /
	// leftmost last makes the LEFTMOST modifier the outermost — matching Solidity's left-to-right
	// modifier evaluation. Forward order reversed the nesting, so a stacked inner modifier's
	// pre/post code escaped an outer modifier's conditional `_;` (e.g. `gated both` ran both's
	// body unconditionally). The viaIR chain builder already iterates in reverse for this reason.
	auto const& _mods = _func.modifiers();
	for (auto _modIt = _mods.rbegin(); _modIt != _mods.rend(); ++_modIt)
	{
		auto const& modInvocation = *_modIt;
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

				// A modifier ARGUMENT can be an arbitrary side-effecting expression:
				// a ternary with a checked/negate branch, a checked op, a short-circuit.
				// SolConditional lowers such a ternary to a branch-gating if/else that
				// assigns its result to a temp (returning a bare temp-READ as the
				// expression) — emitted as a PRE-pending statement. build() leaves those
				// in the context; the inliner must drain them into the modifier body at
				// the binding point. Without this the temp is never assigned and the
				// bound value is garbage — `mod(a > 0 ? a : -a)` collapsed to `-a`,
				// reverting on EVERY call. Found by coverage-guided fuzzing (this inliner
				// was 39.9% line-covered; modifier args with side-effecting exprs unhit).
				m_exprBuilder.appendPendingTo(modBody->body);

				// Storage-pointer params: alias to the original storage location;
				// writes must mutate storage, not a local copy.
				if (param->referenceLocation()
					== solidity::frontend::VariableDeclaration::Location::Storage)
				{
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
					remappedDeclIds.push_back(param->id());
					continue;
				}

				std::string uniqueName = "__mod_" + param->name() + "_" + std::to_string(awst::NameGen::next("ModifierBodyInliner.modCounter"));
				auto* paramType = m_typeMapper.map(param->type());

				argExpr = TypeCoercion::implicitNumericCast(
					std::move(argExpr), paramType, modLoc);
				// `onlyRole(MINTER_ROLE)` binds keccak256(...) — wtype unsized
				// `bytes` — to a `bytes32` param. Bytes are right, label is not,
				// and puya rejects the mismatch outright.
				argExpr = TypeCoercion::relabelUnsizedBytes(
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
						= "__mod_local_" + localDecl->name() + "_" + std::to_string(awst::NameGen::next("ModifierBodyInliner.modCounter"));
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

					// Skip `retvar = 0` for synthetic __mod_retval_* (already 0-init);
					// named return vars can be user-assigned so their `return 0` must emit.
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
			std::string flagName = "__mod_exit_" + std::to_string(awst::NameGen::next("ModifierBodyInliner.modExitCounter"));
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
						// Valued return in a loop/branch: assign to retvar before flag+break.
						// Top-level returns are handled by the deferral pass.
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
					else
						awst::forEachChildBlock(*s, [&](awst::Block& b, bool isLoopBody) {
							replaceReturns(b.body, inLoop || isLoopBody);
						});
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

} // namespace puyasol::builder
