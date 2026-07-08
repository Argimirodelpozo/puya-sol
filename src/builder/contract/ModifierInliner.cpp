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
#include "awst/Clone.h"

#include <libsolidity/ast/ASTVisitor.h>

namespace puyasol::builder
{

void ContractBuilder::inlineModifiers(
	solidity::frontend::FunctionDefinition const& _func,
	std::shared_ptr<awst::Block>& _body
)
{
	// Delegate to the free inlineModifiers in ModifierBodyInliner.cpp via makeFunctionCtx().
	auto ctx = makeFunctionCtx();
	::puyasol::builder::inlineModifiers(ctx, _func, _body);
}

void ContractBuilder::buildModifierChain(
	solidity::frontend::FunctionDefinition const& _func,
	awst::ContractMethod& _method,
	std::string const& _contractName,
	std::vector<std::shared_ptr<awst::Statement>> const& _paramDecodes
)
{
	static int modChainCounter = 0;
	int chainId = modChainCounter++;

	auto const& modifiers = _func.modifiers();
	if (modifiers.empty())
		return;

	std::string baseName = _func.name();
	std::string cref = m_sourceFile + "." + _contractName;

	// Every emitted sub receives the still-ARC4-encoded `__arc4_*` params. Prepend a
	// FRESH clone of the ABI param decodes to any sub that uses them (the body's
	// arithmetic, a modifier's arg expr), so it works on native values. Clone per sub
	// (fresh SingleEvaluation ids) — sharing the same statement nodes across subs aliases
	// them. Decoding in each sub is redundant but idempotent + correct.
	auto prependDecodes = [&](std::shared_ptr<awst::Block> const& _blk) {
		if (_paramDecodes.empty() || !_blk) return;
		std::vector<std::shared_ptr<awst::Statement>> clones;
		clones.reserve(_paramDecodes.size());
		for (auto const& s: _paramDecodes)
			clones.push_back(awst::cloneStmt(s));
		_blk->body.insert(_blk->body.begin(),
			std::make_move_iterator(clones.begin()),
			std::make_move_iterator(clones.end()));
	};

	// Innermost subroutine = original function body.
	std::string bodySubName = baseName + "__body_" + std::to_string(chainId);
	{
		awst::ContractMethod bodySub;
		bodySub.sourceLocation = _method.sourceLocation;
		bodySub.cref = cref;
		bodySub.memberName = bodySubName;
		bodySub.returnType = _method.returnType;
		bodySub.args = _method.args; // same params as outer function
		bodySub.body = _method.body; // move the original function body here
		prependDecodes(bodySub.body);
		bodySub.arc4MethodConfig = std::nullopt; // internal, not ABI-routable
		bodySub.pure = _method.pure;
		m_modifierSubroutines.push_back(std::move(bodySub));
	}

	// Modifier subroutines innermost→outermost, each calling the next.
	std::string nextSubName = bodySubName;

	for (int i = static_cast<int>(modifiers.size()) - 1; i >= 0; --i)
	{
		auto const& modInvocation = modifiers[i];
		auto const* modDef = dynamic_cast<solidity::frontend::ModifierDefinition const*>(
			modInvocation->name().annotation().referencedDeclaration
		);
		if (!modDef)
			continue; // constructor base call — handled elsewhere

		// Resolve virtual override unless this is an explicit A.m base call.
		bool isExplicitBaseModifier = false;
		{
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

		awst::ContractMethod modSub;
		modSub.sourceLocation = makeLoc(modDef->location());
		modSub.cref = cref;
		modSub.memberName = modSubName;
		modSub.returnType = _method.returnType;
		modSub.args = _method.args; // receives same params
		modSub.arc4MethodConfig = std::nullopt; // internal
		modSub.pure = _method.pure;

		auto modBody = awst::makeBlock(modSub.sourceLocation);

		// Decode params first so a modifier arg expr (`mArg(a % 5)`) sees native values.
		prependDecodes(modBody);

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

		// At `_`: call nextSubName and capture return value.
		auto placeholderBlock = awst::makeBlock(modSub.sourceLocation);
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
			auto call = awst::makeSubroutineCall(awst::InstanceMethodTarget{nextSubName}, _method.returnType, modSub.sourceLocation);
			for (auto const& arg: _method.args)
				awst::pushCallArg(call->args, arg.name,
					awst::makeVarExpression(arg.name, arg.wtype, modSub.sourceLocation));

			if (!retVarName.empty())
			{
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

		if (!retVarName.empty()) // zero-init return variable
		{
			auto target = awst::makeVarExpression(retVarName, _method.returnType, modSub.sourceLocation);
			auto zeroVal = StorageMapper::makeDefaultValue(_method.returnType, modSub.sourceLocation);
			auto assign = awst::makeAssignmentStatement(std::move(target), std::move(zeroVal), modSub.sourceLocation);
			modBody->body.push_back(std::move(assign));
		}

		setPlaceholderBody(placeholderBlock);
		auto translatedBody = buildBlock(modDef->body());
		setPlaceholderBody(nullptr);

		if (translatedBody)
		{
			if (!retVarName.empty())
			{
				// Rewrite bare `return;` in modifier body to `return __retval;`.
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

		{
			auto retStmt = awst::makeReturnStatement(nullptr, modSub.sourceLocation);
			if (!retVarName.empty())
			{
				auto var = awst::makeVarExpression(retVarName, _method.returnType, modSub.sourceLocation);
				retStmt->value = std::move(var);
			}
			modBody->body.push_back(std::move(retStmt));
		}

		for (auto declId: remappedDeclIds)
			m_tr->eraseParamRemap(declId);

		modSub.body = modBody;
		m_modifierSubroutines.push_back(std::move(modSub));
		nextSubName = modSubName;
	}

	// Rewrite _method.body to call the outermost modifier subroutine.
	auto entryBody = awst::makeBlock(_method.sourceLocation);

	for (auto const& rp: _func.returnParameters()) // zero-init named return vars
	{
		if (rp->name().empty()) continue;
		auto* rpType = m_typeMapper.map(rp->type());
		auto target = awst::makeVarExpression(rp->name(), rpType, _method.sourceLocation);

		auto zeroVal = StorageMapper::makeDefaultValue(rpType, _method.sourceLocation);
		auto assign = awst::makeAssignmentStatement(std::move(target), std::move(zeroVal), _method.sourceLocation);
		entryBody->body.push_back(std::move(assign));
	}

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
