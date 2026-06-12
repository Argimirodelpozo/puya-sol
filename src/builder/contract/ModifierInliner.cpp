/// @file ModifierInliner.cpp
/// Builds the modifier *call* chain for `function f() m1 m2` — emits
/// `__body`, `__mod0`, `__mod1` subroutines and rewrites `_method.body`
/// to invoke the outermost one. The body-level inliner (the free
/// `inlineModifiers` used by library / free-function translation) lives
/// in `ModifierBodyInliner.cpp`; the `ContractBuilder::inlineModifiers`
/// wrapper at the bottom of this file delegates there so both paths
/// share the same implementation.

#include "builder/contract/ContractBuilder.h"
#include "builder/contract/StateVarWalker.h"
#include "builder/sol-ast/stmts/SolBlock.h"
#include "builder/sol-types/TypeCoercion.h"

#include <libsolidity/ast/ASTVisitor.h>

namespace puyasol::builder
{

void ContractBuilder::inlineModifiers(
	solidity::frontend::FunctionDefinition const& _func,
	std::shared_ptr<awst::Block>& _body
)
{
	// Contract methods and constructors share the free-function modifier
	// inliner in ModifierBodyInliner.cpp. `makeFunctionCtx()` packages
	// this builder's per-function state into the FunctionTranslationCtx
	// the free `inlineModifiers` expects — the same wrapper
	// `ContractBuilder::buildBlock` already routes through. (This routine
	// was previously a ~490-line copy that had drifted out of sync with
	// the free version, most notably lacking its storage-pointer
	// modifier-param handling.)
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
