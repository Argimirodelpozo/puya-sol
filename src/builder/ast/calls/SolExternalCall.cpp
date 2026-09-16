/// @file SolExternalCall.cpp
/// External interface/contract calls via inner app transactions.

#include "builder/ast/calls/SolExternalCall.h"
#include "builder/AwstShorthand.h"
#include "builder/types/CallBoundaryPlan.h"
#include "builder/solc/SolcFacts.h"
#include "builder/storage/slot/EvmSlotLowering.h"
#include "builder/types/ConversionPlan.h"
#include "builder/lowering/abi/AbiEncoderBuilder.h"
#include "builder/lowering/itxn/InnerCallHandlers.h"
#include "builder/lowering/itxn/NativePayment.h"
#include "builder/target/ApplicationTarget.h"
#include "builder/lowering/itxn/ApplicationCall.h"
#include "builder/types/TypeMapper.h"
#include "builder/types/TypeCoercion.h"
#include "Logger.h"

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/ASTUtils.h>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;

std::string SolExternalCall::buildMethodSelector(MemberAccess const& _memberAccess)
{
	auto const* type = dynamic_cast<FunctionType const*>(_memberAccess.annotation().type);
	assert(type);
	return eb::InnerCallHandlers::buildMethodSelector(m_ctx, _memberAccess.memberName(), *type);
}

std::shared_ptr<awst::Expression> SolExternalCall::toAwst()
{
	auto const& funcExpr = funcExpression();
	auto const* memberAccess = SolcFacts::expressionAs<MemberAccess>(&funcExpr);
	if (!memberAccess)
	{
		auto vc = awst::makeVoidConstant(m_loc);
		return vc;
	}

	// `(new C()).stateVar()` deploys the child and calls its auto-getter via
	// inner txn like any other external call. (A former fold to the declared
	// initializer predated real child deployment: it evaluated the initializer
	// in the CALLER's context — `uint x = msg.value - 10` folded to the
	// caller's msg.value — and skipped constructor effects entirely. The
	// `{value:}` variant always took this faithful path, proving it works.)

	// Detect delegatecall to library functions — not supported on AVM
	if (auto const* refDecl = memberAccess->annotation().referencedDeclaration)
	{
		if (auto const* funcDef = dynamic_cast<FunctionDefinition const*>(refDecl))
		{
			auto const* contractDef = funcDef->annotation().contract;
			if (contractDef && contractDef->isLibrary())
			{
				Logger::instance().error(
					"delegatecall to public library function '" + contractDef->name()
					+ "." + funcDef->name() + "' is not supported on AVM. "
					"Use internal library functions instead.", m_loc);
			}
		}
	}

	// {value: V} on a TYPED external call: prepend a PaymentTxn in the same
	// inner group (the callee's msg.value reads the preceding payment) — the
	// same convention as the low-level .call{value:} shapes. This ALSO
	// evaluates a {gas: expr} option for its side effects (the amount itself
	// has no AVM analogue). Previously this path never read the options and
	// the value was SILENTLY DROPPED (callee saw msg.value == 0).
	auto receiver = m_ctx.lowerOperand([&] {
		return ApplicationTarget::resolve(m_ctx.typeMapper.profile(),
			buildExpr(memberAccess->expression()), m_loc);
	}, false);
	auto baseTranslated = m_ctx.emitSequencedOperand(
		std::move(receiver.effects), std::move(receiver.value), true, m_loc);
	auto callValue = extractCallValue();

	// Build selector. The selected contract profile owns the transport; Solidity
	// `abi.*` expression semantics remain canonical independently.
	std::shared_ptr<awst::Expression> selector;
	if (m_ctx.typeMapper.profile().contractAbi == ContractAbi::Evm)
	{
		auto const* functionType = dynamic_cast<FunctionType const*>(
			memberAccess->annotation().type);
		if (!functionType)
		{
			Logger::instance().error(
				"cannot derive Solidity selector for external call", m_loc);
			return awst::makeVoidConstant(m_loc);
		}
		selector = awst::makeBytesConstant(
			builder::SolcFacts::externalSelector(*functionType), m_loc,
			awst::BytesEncoding::Base16, awst::WType::bytesType());
	}
	else
		selector = awst::makeMethodConstant(
			buildMethodSelector(*memberAccess), awst::WType::bytesType(), m_loc);

	auto const* functionType = dynamic_cast<FunctionType const*>(memberAccess->annotation().type);
	assert(functionType);
	auto const& paramSolTypes = functionType->parameterTypes();
	bool const evm = m_ctx.typeMapper.profile().contractAbi == ContractAbi::Evm;
	auto values = CallOperands::build(m_ctx, m_call, m_loc,
		[&](Expression const& source, size_t i) {
			auto value = buildExpr(source);
			auto const* param = paramSolTypes.at(i);
			value = EvmSlotLowering::materializeRefValue(m_ctx, std::move(value),
				source.annotation().type, m_ctx.typeMapper.map(param), m_loc);
			return ConversionPlan{source.annotation().type, param, m_ctx.typeMapper.map(param),
				ConversionPlan::Context::AbiArgument}.emit(std::move(value), m_loc);
		});
	auto argsTuple = awst::makeTupleExpression(nullptr, m_loc);
	argsTuple->items.push_back(std::move(selector));
	if (evm)
		argsTuple->items.push_back(eb::AbiEncoderBuilder::encodeValuesAsEvmAbi(
			m_ctx, paramSolTypes, std::move(values), m_loc));
	else
		for (size_t i = 0; i < values.size(); ++i)
			argsTuple->items.push_back(eb::InnerCallHandlers::encodeArgToBytes(m_ctx,
				std::move(values[i]), paramSolTypes[i], paramSolTypes[i], m_loc));
	std::vector<awst::WType const*> argTypes;
	for (auto const& item: argsTuple->items)
		argTypes.push_back(item->wtype);
	argsTuple->wtype = m_ctx.typeMapper.createType<awst::WTuple>(
		std::move(argTypes), std::nullopt);
	// Convert receiver to app ID
	auto appId = ApplicationTarget::requireApplication(std::move(baseTranslated), m_loc);

	// The {value:} payment pays the called app's ESCROW, derived from the
	// same app id (a contract-value address paid verbatim lands on a keyless
	// zero-padded pseudo-account). app_params_get needs the app available —
	// the same requirement the inner ApplicationCall already imposes.
	std::shared_ptr<awst::Expression> payTxn;
	if (callValue)
	{
		appId = awst::makeEvalOnce(std::move(appId), m_loc);
		payTxn = buildNativePayment(m_ctx.typeMapper.profile(), m_ctx.preEffects(),
			awst::makeAsApplication(appId, m_loc), std::move(callValue), m_loc);
	}

	auto payload = ApplicationCall::submit(m_ctx.typeMapper,
		awst::makeAsApplication(std::move(appId), m_loc), std::move(argsTuple),
		std::move(payTxn), m_loc, m_ctx.preEffects());
	auto const* resultType = m_ctx.typeMapper.map(m_call.annotation().type);
	if (resultType == awst::WType::voidType()) return awst::makeVoidConstant(m_loc);
	return decodeExternalCallResult(m_ctx.typeMapper, std::move(payload),
		functionType->returnParameterTypes(), resultType, m_loc, m_ctx.preEffects());
}

} // namespace puyasol::builder::sol_ast
