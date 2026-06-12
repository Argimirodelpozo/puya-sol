#include "builder/sol-ast/calls/SolRequireAssert.h"
#include "builder/sol-ast/calls/RevertBlob.h"
#include "builder/sol-eb/AbiEncoderBuilder.h"

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
			// Solidity evaluates require's error args eagerly — even on the
			// success path (see errors/require_error_evaluation_order_1.sol).
			// Build the EVM-shaped payload — selector ++ abi.encode(args) —
			// and hoist it to a temp NOW: that evaluates each arg exactly
			// once, eagerly (replacing the old bare-prepend mechanism), while
			// the log itself only fires on failure via the conditional below.
			if (isCustomError)
			{
				auto const* errorDef =
					dynamic_cast<solidity::frontend::ErrorDefinition const*>(
						solidity::frontend::ASTNode::referencedDeclaration(
							errorCall->expression()));
				if (errorDef)
				{
					// AVM-convention selector (sha512_256, like events/methods)
					// via MethodConstant — see SolRevertStatement.
					auto sig = errorDef->functionType(true)->externalSignature();
					std::shared_ptr<awst::Expression> blob =
						awst::makeMethodConstant(sig, awst::WType::bytesType(), m_loc);
					if (!errorCall->arguments().empty())
						blob = awst::makeConcat(
							std::move(blob),
							eb::AbiEncoderBuilder::encodeArgsHeadTail(
								m_ctx, *errorCall, 0, m_loc),
							m_loc);
					static int s_reqErrBlobCounter = 0;
					std::string tmpName = "__require_err_blob_"
						+ std::to_string(++s_reqErrBlobCounter);
					m_ctx.prePendingStatements.push_back(awst::makeAssignmentStatement(
						awst::makeVarExpression(tmpName, awst::WType::bytesType(), m_loc),
						std::move(blob), m_loc));
					revertBlob = awst::makeVarExpression(
						tmpName, awst::WType::bytesType(), m_loc);
				}
				else
					// No resolvable ErrorDefinition: keep the legacy eager
					// arg-evaluation so side effects still land.
					for (auto const& a : errorCall->arguments())
					{
						auto argExpr = buildExpr(*a);
						if (argExpr && argExpr->wtype && argExpr->wtype != awst::WType::voidType())
						{
							auto stmt = awst::makeExpressionStatement(std::move(argExpr), m_loc);
							m_ctx.prePendingStatements.push_back(std::move(stmt));
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
				// Runtime message (e.g. `require(ok, someStringVar)`): build the
				// Error(string) payload at runtime. Evaluated only on failure —
				// EVM technically evaluates the message eagerly, but the prior
				// lowering DISCARDED a non-constant message entirely, so this
				// strictly improves fidelity.
				revertBlob = makeErrorStringRevertBlob(std::move(msgExpr), m_loc);
			}
		}
	}

	// `assert(cond)` reverts with Panic(0x01) on EVM.
	if (isAssertBuiltin && !revertBlob)
		revertBlob = awst::makeBytesConstant(panicRevertBlobBytes(0x01), m_loc);

	// With a structured payload, lower as
	//   if (!cond) { log(blob); fail }
	// so clients (and the harness via simulate) read EVM-shaped revert data
	// from the failing txn's last log. A message-less `require(cond)` keeps
	// the plain assert — EVM's revert data is empty there too. Custom errors
	// keep the existing path (selector+args payload is a follow-up).
	if (revertBlob && condition)
	{
		auto failBlock = awst::makeBlock(m_loc);
		failBlock->body.push_back(makeRevertLogStmt(std::move(revertBlob), m_loc));
		failBlock->body.push_back(awst::makeExpressionStatement(
			awst::makeAssert(awst::makeFalse(m_loc), m_loc, std::move(message)), m_loc));
		m_ctx.pendingStatements.push_back(awst::makeIfElse(
			awst::makeNot(std::move(condition), m_loc), std::move(failBlock), nullptr, m_loc));
		return awst::makeVoidConstant(m_loc);
	}
	if (revertBlob)
	{
		// Condition-less (always-fail) shape with a payload.
		m_ctx.prePendingStatements.push_back(
			makeRevertLogStmt(std::move(revertBlob), m_loc));
	}

	return awst::makeAssert(std::move(condition), m_loc, std::move(message));
}

} // namespace puyasol::builder::sol_ast
