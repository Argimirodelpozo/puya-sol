#include "builder/ContractBuilder.h"
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

void ContractBuilder::inlineModifiers(
	solidity::frontend::FunctionDefinition const& _func,
	std::shared_ptr<awst::Block>& _body
)
{
	static int modCounter = 0;
	static int modRetvalCounter = 0;

	// Extract named return var init statements from the body and hoist them
	// BEFORE modifier arg evaluation. Without this, modifier args like
	// m1(x = 2) m2(y = 3) would have y overwritten by the "y = 0" init
	// that's inside the innermost placeholder body.
	std::set<std::string> returnParamNames;
	for (auto const& rp : _func.returnParameters())
		if (!rp->name().empty())
			returnParamNames.insert(rp->name());

	// Unnamed returns: synthesise return vars so `return expr;` can be
	// rewritten into `__mod_retval_N = expr;` (in placeholder) + deferred
	// `return __mod_retval_N;` — otherwise the expr would evaluate *after*
	// post-`_` modifier code has run (e.g. `a -= b;` in `mod(x)`), returning
	// a stale value. Covers both single-return and multi-return unnamed.
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
		// Scan the prefix of the body for compiler-inserted named-return inits.
		// Skip benign non-return-var prefix stmts (ExpressionStatement from
		// ensure_budget/ABI asserts, and parameter narrowing like `a = a & 0xff`)
		// but stop at control flow or any assignment that references a return
		// var in its value to avoid reordering user-written logic.
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
				// Assignment to a non-return-var (e.g. parameter narrowing
				// `a = a & 0xff`): skip, keep scanning.
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
	// Default-init the synthetic return vars so the deferred `return` always
	// reads valid values, even on execution paths that don't reach the split
	// assignment (e.g. early revert inside the modifier).
	for (auto const& [n, t]: syntheticRets)
	{
		auto synthInit = std::make_shared<awst::AssignmentStatement>();
		synthInit->sourceLocation = makeLoc(_func.location());
		auto target = awst::makeVarExpression(n, t, synthInit->sourceLocation);
		synthInit->target = std::move(target);
		synthInit->value = StorageMapper::makeDefaultValue(t, synthInit->sourceLocation);
		hoistedInits.push_back(std::move(synthInit));
	}

	// For each modifier invocation, wrap the function body
	for (auto const& modInvocation: _func.modifiers())
	{
		auto const* modDef = dynamic_cast<solidity::frontend::ModifierDefinition const*>(
			modInvocation->name().annotation().referencedDeclaration
		);

		if (!modDef)
			continue;

		// Resolve virtual overrides — but NOT for explicit base modifier calls (A.m).
		{
			bool isExplicit = modInvocation->name().path().size() > 1;
			if (m_currentContract && !isExplicit)
			{
				std::string modName = modDef->name();
				for (auto const* base: m_currentContract->annotation().linearizedBaseContracts)
					for (auto const* mod: base->functionModifiers())
						if (mod->name() == modName) { modDef = mod; goto modResolved; }
				modResolved:;
			}
		}

		// Translate modifier body, replacing `_` (PlaceholderStatement) with the original body
		auto modBody = std::make_shared<awst::Block>();
		modBody->sourceLocation = makeLoc(modDef->location());

		if (modDef->body().statements().empty())
			continue;

		// Bind modifier arguments to unique local variables.
		// e.g. onlyRole(getRoleAdmin(role)) → __mod_role_N = getRoleAdmin(role)
		auto const* args = modInvocation->arguments();
		auto const& params = modDef->parameters();
		std::vector<int64_t> remappedDeclIds;

		if (args && !args->empty())
		{
			auto modLoc = makeLoc(modInvocation->location());
			for (size_t i = 0; i < args->size() && i < params.size(); ++i)
			{
				auto const& param = params[i];
				std::string uniqueName = "__mod_" + param->name() + "_" + std::to_string(modCounter++);
				auto* paramType = m_typeMapper.map(param->type());

				// Translate the argument expression (e.g. getRoleAdmin(role))
				auto argExpr = m_exprBuilder->build(*(*args)[i]);
				if (!argExpr)
					continue;

				// Cast to parameter type if needed
				argExpr = TypeCoercion::implicitNumericCast(
					std::move(argExpr), paramType, modLoc
				);

				// Create assignment: __mod_role_N = <evaluated arg>
				auto target = awst::makeVarExpression(uniqueName, paramType, modLoc);

				auto assignment = awst::makeAssignmentStatement(target, std::move(argExpr), modLoc);
				modBody->body.push_back(std::move(assignment));

				// Register remap so modifier body references resolve to the unique name
				m_exprBuilder->paramRemaps[param->id()] = puyasol::builder::eb::ParamRemap{uniqueName, paramType};
				remappedDeclIds.push_back(param->id());
			}
		}

		// Rename modifier-body local variables uniquely per application so that
		// the same modifier applied multiple times (e.g. `mod(2) mod(5)`) does
		// not share storage slots for its own local variables.
		//
		// Skip the rename when the modifier body contains inline assembly:
		// Yul identifiers reference their original Solidity names (see
		// SolInlineAssembly.cpp and AssemblyBuilder::buildIdentifier), so a
		// renamed declaration would decouple the assembly's writes from the
		// surrounding Solidity reads.
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
					m_exprBuilder->paramRemaps[localDecl->id()] = puyasol::builder::eb::ParamRemap{uniqueName, localType};
					remappedDeclIds.push_back(localDecl->id());
				}
			}
		}

		// Separate the function body into:
		// 1. Named return var initializations (hoisted before modifier)
		// 2. Placeholder body (the `_;` replacement)
		// 3. Deferred return (appended after modifier)
		//
		// Named return vars must be initialized before the modifier body so
		// that multiple `_;` invocations (e.g. in loops) share the same
		// variable rather than re-initializing it each time.
		auto placeholderBody = std::make_shared<awst::Block>();
		placeholderBody->sourceLocation = _body->sourceLocation;
		std::shared_ptr<awst::Statement> deferredReturn;

		// Track which named return vars have already been hoisted (only hoist
		// the first assignment — the default init — not subsequent assignments
		// like `r = true` from `return true;`)
		std::set<std::string> hoistedReturnVars;

		for (auto const& bodyStmt: _body->body)
		{
			if (auto const* retStmt = dynamic_cast<awst::ReturnStatement const*>(bodyStmt.get()))
			{
				// For named returns: split `return expr;` into `r = expr;` (in
				// placeholder body) + `return r;` (deferred). This ensures the
				// modifier controls whether the assignment executes.
				if (returnParamNames.size() == 1 && retStmt->value)
				{
					auto const& retName = *returnParamNames.begin();
					// Check if the return value is NOT just a reference to the
					// named return var itself (avoid redundant r = r;)
					bool isJustRetVar = false;
					if (auto const* varRef = dynamic_cast<awst::VarExpression const*>(retStmt->value.get()))
						isJustRetVar = (varRef->name == retName);

					if (!isJustRetVar)
					{
						// Create assignment: r = expr
						auto target = awst::makeVarExpression(retName, retStmt->value->wtype, retStmt->sourceLocation);

						auto assign = awst::makeAssignmentStatement(std::move(target), retStmt->value, retStmt->sourceLocation);
						placeholderBody->body.push_back(std::move(assign));
					}

					// Create deferred return: return r
					auto retVar = awst::makeVarExpression(retName, retStmt->value->wtype, retStmt->sourceLocation);

					auto deferRet = awst::makeReturnStatement(std::move(retVar), retStmt->sourceLocation);
					deferredReturn = std::move(deferRet);
				}
				else if (syntheticRets.size() > 1 && retStmt->value)
				{
					// Multi-return unnamed: capture each component now so post-`_`
					// modifier code can't mutate the return value via storage writes.
					auto const* tupleVal = dynamic_cast<awst::TupleExpression const*>(retStmt->value.get());
					if (tupleVal && tupleVal->items.size() == syntheticRets.size())
					{
						for (size_t i = 0; i < syntheticRets.size(); ++i)
						{
							auto target = awst::makeVarExpression(
								syntheticRets[i].first, syntheticRets[i].second, retStmt->sourceLocation);
							auto assign = awst::makeAssignmentStatement(
								std::move(target), tupleVal->items[i], retStmt->sourceLocation);
							placeholderBody->body.push_back(std::move(assign));
						}
					}
					else
					{
						// Non-tuple value returning a tuple (e.g. multi-return call):
						// use a TupleExpression target to destructure via assignment.
						auto tupleTarget = std::make_shared<awst::TupleExpression>();
						tupleTarget->sourceLocation = retStmt->sourceLocation;
						std::vector<awst::WType const*> tupleTypes;
						for (auto const& [n, t]: syntheticRets)
						{
							tupleTarget->items.push_back(
								awst::makeVarExpression(n, t, retStmt->sourceLocation));
							tupleTypes.push_back(t);
						}
						tupleTarget->wtype = m_typeMapper.createType<awst::WTuple>(
							std::move(tupleTypes), std::nullopt);
						auto assign = awst::makeAssignmentStatement(
							std::move(tupleTarget), retStmt->value, retStmt->sourceLocation);
						placeholderBody->body.push_back(std::move(assign));
					}

					// Rebuild return as a tuple of the captured vars
					auto deferTuple = std::make_shared<awst::TupleExpression>();
					deferTuple->sourceLocation = retStmt->sourceLocation;
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
				{
					deferredReturn = bodyStmt;
				}
			}
			else if (!returnParamNames.empty())
			{
				// Check if this is a named return var initialization (first assignment only)
				auto const* assign = dynamic_cast<awst::AssignmentStatement const*>(bodyStmt.get());
				auto const* targetVar = assign
					? dynamic_cast<awst::VarExpression const*>(assign->target.get())
					: nullptr;
				if (targetVar && returnParamNames.count(targetVar->name)
					&& !hoistedReturnVars.count(targetVar->name))
				{
					// Hoist: put initialization before the modifier body
					modBody->body.push_back(bodyStmt);
					hoistedReturnVars.insert(targetVar->name);
				}
				else
				{
					placeholderBody->body.push_back(bodyStmt);
				}
			}
			else
			{
				placeholderBody->body.push_back(bodyStmt);
			}
		}

		// Translate the entire modifier body through the statement builder.
		// The `_;` (PlaceholderStatement) will be replaced with the function body
		// wherever it appears — including inside loops and conditionals.
		setPlaceholderBody(placeholderBody);
		auto translatedModBody = buildBlock(modDef->body());
		setPlaceholderBody(nullptr);

		// Modifier `return` should exit the modifier scope, not the function.
		// Strategy: use a flag __mod_exit_N. Replace return → { flag=true; break; }
		// After each inner loop, check flag and break again if set.
		// Wrap entire modifier body in while(true){...;break;} so outer break works.
		if (translatedModBody)
		{
			static int modExitCounter = 0;
			std::string flagName = "__mod_exit_" + std::to_string(modExitCounter++);
			auto flagLoc = translatedModBody->sourceLocation;

			// Helper: create flag = true assignment
			auto makeFlagSet = [&]() -> std::shared_ptr<awst::Statement> {
				auto target = awst::makeVarExpression(flagName, awst::WType::boolType(), flagLoc);
				auto assign = awst::makeAssignmentStatement(std::move(target), awst::makeBoolConstant(true, flagLoc), flagLoc);
				return assign;
			};

			// Helper: create LoopExit
			auto makeBreak = [&]() -> std::shared_ptr<awst::Statement> {
				auto le = std::make_shared<awst::LoopExit>();
				le->sourceLocation = flagLoc;
				return le;
			};

			// Helper: create if(flag) break;
			auto makeFlagCheck = [&]() -> std::shared_ptr<awst::Statement> {
				auto cond = awst::makeVarExpression(flagName, awst::WType::boolType(), flagLoc);
				auto branchBody = std::make_shared<awst::Block>();
				branchBody->sourceLocation = flagLoc;
				branchBody->body.push_back(makeBreak());
				auto ifStmt = std::make_shared<awst::IfElse>();
				ifStmt->sourceLocation = flagLoc;
				ifStmt->condition = std::move(cond);
				ifStmt->ifBranch = std::move(branchBody);
				return ifStmt;
			};

			// Replace ReturnStatement → { flag=true; break; }
			// Inside loops: also add flag check after the loop
			bool hasReturnInLoop = false;
			std::function<void(std::vector<std::shared_ptr<awst::Statement>>&, bool)> replaceReturns;
			replaceReturns = [&](std::vector<std::shared_ptr<awst::Statement>>& stmts, bool inLoop) {
				for (size_t i = 0; i < stmts.size(); ++i)
				{
					auto& s = stmts[i];
					if (dynamic_cast<awst::ReturnStatement*>(s.get()))
					{
						// Replace return with { flag=true; break; }
						auto block = std::make_shared<awst::Block>();
						block->sourceLocation = s->sourceLocation;
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

			// After each WhileLoop that contains a return, insert flag check
			if (hasReturnInLoop)
			{
				std::function<void(std::vector<std::shared_ptr<awst::Statement>>&)> insertFlagChecks;
				insertFlagChecks = [&](std::vector<std::shared_ptr<awst::Statement>>& stmts) {
					for (size_t i = 0; i < stmts.size(); ++i)
					{
						if (dynamic_cast<awst::WhileLoop*>(stmts[i].get()))
						{
							// Insert flag check after the loop
							stmts.insert(stmts.begin() + i + 1, makeFlagCheck());
							++i; // skip inserted
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

			// Initialize flag: __mod_exit_N = false
			auto flagInit = std::make_shared<awst::AssignmentStatement>();
			flagInit->sourceLocation = flagLoc;
			auto flagTarget = awst::makeVarExpression(flagName, awst::WType::boolType(), flagLoc);
			flagInit->target = std::move(flagTarget);
			flagInit->value = awst::makeBoolConstant(false, flagLoc);
			modBody->body.push_back(std::move(flagInit));

			// Wrap in while(true) { ...body...; break; }
			auto wrapperLoop = std::make_shared<awst::WhileLoop>();
			wrapperLoop->sourceLocation = flagLoc;
			wrapperLoop->condition = awst::makeBoolConstant(true, flagLoc);

			auto loopBody = std::make_shared<awst::Block>();
			loopBody->sourceLocation = flagLoc;
			for (auto& stmt: translatedModBody->body)
				loopBody->body.push_back(std::move(stmt));
			loopBody->body.push_back(makeBreak()); // always exit after one pass
			wrapperLoop->loopBody = std::move(loopBody);

			modBody->body.push_back(std::move(wrapperLoop));
		}
		else
		{
			// Fallback: copy body as-is
			for (auto const& bodyStmt: _body->body)
				modBody->body.push_back(bodyStmt);
		}

		// Append the deferred return after the modifier body
		if (deferredReturn)
			modBody->body.push_back(std::move(deferredReturn));

		// Unregister remaps so they don't affect subsequent code
		for (auto declId: remappedDeclIds)
			m_exprBuilder->paramRemaps.erase(declId);

		_body = modBody;
	}

	// Prepend hoisted return var inits before the modifier chain
	if (!hoistedInits.empty())
	{
		_body->body.insert(
			_body->body.begin(),
			std::make_move_iterator(hoistedInits.begin()),
			std::make_move_iterator(hoistedInits.end()));
	}
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
			for (auto const* base: m_currentContract->annotation().linearizedBaseContracts)
				for (auto const* mod: base->functionModifiers())
					if (mod->name() == modName) { modDef = mod; goto foundMostDerived; }
			foundMostDerived:;
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
		auto modBody = std::make_shared<awst::Block>();
		modBody->sourceLocation = modSub.sourceLocation;

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

				m_exprBuilder->paramRemaps[param->id()] = puyasol::builder::eb::ParamRemap{uniqueName, paramType};
				remappedDeclIds.push_back(param->id());
			}
		}

		// Set placeholder body: at `_;`, call the next subroutine
		// Build a block that calls nextSubName and assigns return value
		auto placeholderBlock = std::make_shared<awst::Block>();
		placeholderBlock->sourceLocation = modSub.sourceLocation;

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
			auto call = std::make_shared<awst::SubroutineCallExpression>();
			call->sourceLocation = modSub.sourceLocation;
			call->wtype = _method.returnType;
			call->target = awst::InstanceMethodTarget{nextSubName};
			for (auto const& arg: _method.args)
			{
				awst::CallArg ca;
				ca.name = arg.name;
				auto varRef = awst::makeVarExpression(arg.name, arg.wtype, modSub.sourceLocation);
				ca.value = std::move(varRef);
				call->args.push_back(std::move(ca));
			}

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
			m_exprBuilder->paramRemaps.erase(declId);

		modSub.body = modBody;
		m_modifierSubroutines.push_back(std::move(modSub));
		nextSubName = modSubName;
	}

	// Step 3: Replace _method.body with a call to the outermost modifier subroutine
	auto entryBody = std::make_shared<awst::Block>();
	entryBody->sourceLocation = _method.sourceLocation;

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
	auto call = std::make_shared<awst::SubroutineCallExpression>();
	call->sourceLocation = _method.sourceLocation;
	call->wtype = _method.returnType;
	call->target = awst::InstanceMethodTarget{nextSubName};
	for (auto const& arg: _method.args)
	{
		awst::CallArg ca;
		ca.name = arg.name;
		auto varRef = awst::makeVarExpression(arg.name, arg.wtype, _method.sourceLocation);
		ca.value = std::move(varRef);
		call->args.push_back(std::move(ca));
	}

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
