#include "builder/sol-ast/calls/SolRequireAssert.h"
#include "awst/NameGen.h"
#include "builder/SelectorSemantics.h"
#include "builder/sol-ast/calls/RevertBlob.h"
#include "builder/abi/AbiEncoderBuilder.h"
#include "builder/sol-types/SolcConstFold.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder::sol_ast
{

SolRequireAssert::SolRequireAssert(
	eb::ContractContext& _ctx,
	solidity::frontend::FunctionCall const& _call)
	: SolFunctionCall(_ctx, _call)
{
}

std::shared_ptr<awst::Expression> SolRequireAssert::toAwst()
{
	auto const& args = m_call.arguments();
	std::shared_ptr<awst::Expression> condition;
	std::optional<std::string> message;
	std::shared_ptr<awst::Expression> revertBlob;

	// `assert(cond)` (vs `require`): EVM reverts with Panic(0x01).
	bool isAssertBuiltin = false;
	if (auto const* ft = dynamic_cast<solidity::frontend::FunctionType const*>(
			m_call.expression().annotation().type))
		isAssertBuiltin = ft->kind() == solidity::frontend::FunctionType::Kind::Assert;

	if (!args.empty())
		condition = buildExpr(*args[0]);

	if (args.size() > 1)
	{
		// Custom error constructor: require(cond, Errors.Foo(args...))
		bool isCustomError = false;
		if (auto const* errorCall = dynamic_cast<solidity::frontend::FunctionCall const*>(args[1].get()))
		{
			auto const& errExpr = errorCall->expression();
			if (auto const* ma = dynamic_cast<solidity::frontend::MemberAccess const*>(&errExpr))
			{
				message = ma->memberName();
				isCustomError = true;
			}
			else if (auto const* id = dynamic_cast<solidity::frontend::Identifier const*>(&errExpr))
			{
				message = id->name();
				isCustomError = true;
			}
			// Eager eval: require's error args evaluate even on success
			// (errors/require_error_evaluation_order_1.sol). Hoist to temp now;
			// log fires on failure only.
			if (isCustomError)
			{
				auto const* errorDef =
					dynamic_cast<solidity::frontend::ErrorDefinition const*>(
						solidity::frontend::ASTNode::referencedDeclaration(
							errorCall->expression()));
				if (errorDef)
				{
					auto const* errorType = errorDef->functionType(true);
					auto sig = errorType->externalSignature();
					std::shared_ptr<awst::Expression> blob =
						builder::SelectorSemantics::functionSelector(
							m_ctx, *errorType, sig, m_loc);
					auto const errorArgs = errorCall->sortedArguments();
					if (!errorArgs.empty())
						// selector ++ ARC4(args) at declared param types ([[abi-arc4-migration]]);
						// Error(string)/Panic stay EVM-literal (errorString path).
						blob = awst::makeConcat(
							std::move(blob),
							eb::AbiEncoderBuilder::arc4EncodeArgsAtParamTypes(
								m_ctx, errorArgs,
								errorDef->functionType(true)->parameterTypes(), m_loc),
							m_loc);
					std::string tmpName = "__require_err_blob_"
						+ std::to_string((awst::NameGen::next("SolRequireAssert.s_reqErrBlobCounter") + 1));
					m_ctx.preEffects().push_back(awst::makeAssignmentStatement(
						awst::makeVarExpression(tmpName, awst::WType::bytesType(), m_loc),
						std::move(blob), m_loc));
					revertBlob = awst::makeVarExpression(
						tmpName, awst::WType::bytesType(), m_loc);
				}
				else
					// No ErrorDefinition: evaluate args for side effects only.
					for (auto const& a : errorCall->sortedArguments())
					{
						auto argExpr = buildExpr(*a);
						if (argExpr && argExpr->wtype && argExpr->wtype != awst::WType::voidType())
						{
							auto stmt = awst::makeExpressionStatement(std::move(argExpr), m_loc);
							m_ctx.preEffects().push_back(std::move(stmt));
						}
					}
			}
		}
		if (!isCustomError)
		{
			auto msgExpr = buildExpr(*args[1]);
			if (auto const* sc = dynamic_cast<awst::StringConstant const*>(msgExpr.get()))
			{
				message = sc->value;
				revertBlob = awst::makeBytesConstant(
					errorStringRevertBlobBytes(sc->value), m_loc);
			}
			else
			{
				message = "assertion failed";
				// Runtime message: build Error(string) payload at runtime.
				// (Prior lowering discarded non-constant messages; this improves fidelity.)
				revertBlob = makeErrorStringRevertBlob(std::move(msgExpr), m_loc);
			}
		}
	}

	// `assert(cond)` reverts with Panic(0x01) on EVM.
	if (isAssertBuiltin && !revertBlob)
		revertBlob = awst::makeBytesConstant(panicRevertBlobBytes(0x01), m_loc);

	// With a structured payload:
	//   if (!cond) { log(blob) }   // pre-effect
	//   assert(cond, msg)
	// Assert stays on the native node — assert inside if-branch broke puya's
	// explicit-assert accounting on constant-folded conditions. SE-wrap the
	// condition so a side-effecting cond evaluates once (gate lowers first).
	if (revertBlob && condition)
	{
		// Constant conditions: lower directly — constant-gated if/assert
		// trips puya's explicit-assert accounting.
		if (auto const* bc = dynamic_cast<awst::BoolConstant const*>(condition.get()))
		{
			if (bc->value)
				// require(true, E(args)): args already evaluated; nothing to check.
				return awst::makeVoidConstant(m_loc);
			// require(false, E(args)): unconditional log + fail.
			m_ctx.preEffects().push_back(
				makeRevertLogStmt(std::move(revertBlob), m_loc));
			auto failNode = awst::makeAssert(
				awst::makeFalse(m_loc), m_loc, std::move(message));
			failNode->isExplicit = false;
			return failNode;
		}
		// The condition is referenced twice below (the !cond gate + the assert).
		// Skip the EvalOnce wrapper when solc marked the condition PURE and it
		// lowered to a leaf var — re-reading a variable twice is cheaper than
		// the SE scratch traffic, and purity makes the duplication sound
		// (fable-review item 2; isPure licenses DUPLICATION only, never
		// elision — see SolcConstFold::isEffectFree).
		bool pureLeafCond = !args.empty()
			&& builder::SolcConstFold::isEffectFree(*args[0])
			&& dynamic_cast<awst::VarExpression const*>(condition.get()) != nullptr;
		if (!pureLeafCond)
			condition = awst::makeEvalOnce(std::move(condition), m_loc);
		auto logBlock = awst::makeBlock(m_loc);
		logBlock->body.push_back(makeRevertLogStmt(std::move(revertBlob), m_loc));
		m_ctx.preEffects().push_back(awst::makeIfElse(
			awst::makeNot(condition, m_loc), std::move(logBlock), nullptr, m_loc));
		auto assertNode = awst::makeAssert(
			std::move(condition), m_loc, std::move(message));
		assertNode->isExplicit = false;
		return assertNode;
	}
	if (revertBlob)
	{
		// Condition-less (always-fail) shape with a payload.
		m_ctx.preEffects().push_back(
			makeRevertLogStmt(std::move(revertBlob), m_loc));
		auto failNode = awst::makeAssert(
			std::move(condition), m_loc, std::move(message));
		failNode->isExplicit = false;
		return failNode;
	}

	return awst::makeAssert(std::move(condition), m_loc, std::move(message));
}

} // namespace puyasol::builder::sol_ast
