/// @file InnerCallShapes.cpp
/// Raw application-call transport and precompile dispatch.

#include "builder/lowering/itxn/InnerCallHandlers.h"
#include "builder/lowering/itxn/NativePayment.h"
#include "builder/lowering/itxn/ApplicationCall.h"
#include "builder/AwstShorthand.h"
#include "builder/target/EvmFeaturePolicy.h"
#include "builder/lowering/itxn/InnerCallInternal.h"
#include "builder/types/TypeMapper.h"
#include "Logger.h"
#include "builder/lowering/itxn/Precompile.h"

namespace puyasol::builder::eb
{

std::unique_ptr<InstanceBuilder> InnerCallHandlers::submitAppCall(
	ContractContext& _ctx,
	std::shared_ptr<awst::Expression> _receiver,
	std::shared_ptr<awst::Expression> _argsTuple,
	std::shared_ptr<awst::Expression> _callValue,
	awst::SourceLocation const& _loc)
{
	// Receiver feeds both the payment's Receiver and the app id derivation.
	std::shared_ptr<awst::Expression> payTxn;
	if (_callValue)
	{
		_receiver = awst::makeEvalOnce(_receiver, _loc);
		payTxn = buildNativePayment(_ctx.typeMapper.profile(), _ctx.preEffects(),
			_receiver, std::move(_callValue), _loc);
	}
	auto payload = ApplicationCall::submit(_ctx.typeMapper, std::move(_receiver),
		std::move(_argsTuple), std::move(payTxn), _loc, _ctx.preEffects());
	return std::make_unique<GenericResultBuilder>(_ctx,
		makeBoolBytesTuple(true, std::move(payload), _loc));
}

// ── .call(rawBytes) → inner app call ──

std::unique_ptr<InstanceBuilder> InnerCallHandlers::handleCallWithRawData(
	ContractContext& _ctx,
	std::shared_ptr<awst::Expression> _receiver,
	std::shared_ptr<awst::Expression> _dataBytes,
	std::shared_ptr<awst::Expression> _callValue,
	awst::SourceLocation const& _loc)
{
	EvmFeaturePolicy::report(
		EvmFeature::LowLevelCallOutcome, _ctx.typeMapper.profile(), _loc);
	if (_dataBytes->wtype == awst::WType::stringType())
	{
		auto cast = awst::makeAsBytes(std::move(_dataBytes), _loc);
		_dataBytes = std::move(cast);
	}

	// Opaque payload splits losslessly into selector + canonical body for an EVM
	// profile. An ARC4-profile target cannot generically reconstruct individual
	// ARC4 ApplicationArgs from an opaque EVM head/tail blob.
	if (_ctx.typeMapper.profile().contractAbi == ContractAbi::Arc4)
		Logger::instance().warning(
			"low-level .call(data) with an opaque payload: forwarding "
			"[selector, rest] to the target's EVM compatibility route or raw "
			"fallback. Native ARC4 arguments cannot be inferred from this blob; "
			"use --contract-abi evm when every public method must support "
			"canonical Solidity calldata.", _loc);
	std::shared_ptr<awst::Expression> payment;
	if (_callValue)
		payment = buildNativePayment(_ctx.typeMapper.profile(), _ctx.preEffects(),
			_receiver, std::move(_callValue), _loc);
	auto result = ApplicationCall::submitRaw(_ctx.typeMapper, std::move(_receiver),
		std::move(_dataBytes), std::move(payment), _loc, _ctx.preEffects());
	return std::make_unique<GenericResultBuilder>(_ctx, makeBoolBytesTuple(true, std::move(result), _loc));
}

std::unique_ptr<InstanceBuilder> InnerCallHandlers::handleCallWithEmptyData(
	ContractContext& _ctx,
	std::shared_ptr<awst::Expression> _receiver,
	awst::SourceLocation const& _loc)
{
	// Solc's dispatcher runs receive() for calldatasize==0 (fallback when no
	// receive exists) even at zero value — the callee EXECUTES. A zero-arg
	// inner app call reaches the EVM entry router's NumAppArgs==0 arm, which
	// is that dispatch. Non-app receivers (EVM: a silent success on an EOA)
	// fail the inner txn — the LowLevelCallOutcome adaptation applies.
	EvmFeaturePolicy::report(
		EvmFeature::LowLevelCallOutcome, _ctx.typeMapper.profile(), _loc);
	return submitAppCall(_ctx, std::move(_receiver), nullptr, nullptr, _loc);
}

// ── .staticcall(data) precompile routing ──

std::unique_ptr<InstanceBuilder> InnerCallHandlers::handleStaticCallPrecompile(
	ContractContext& _ctx,
	uint64_t _precompileAddr,
	std::shared_ptr<awst::Expression> _inputData,
	awst::SourceLocation const& _loc)
{
	auto result = evaluatePrecompile(_ctx.typeMapper, _precompileAddr, std::move(_inputData),
		_loc, _ctx.preEffects());
	if (!result)
		return std::make_unique<GenericResultBuilder>(_ctx, makeBoolBytesTupleEmpty(_loc));
	return std::make_unique<GenericResultBuilder>(_ctx, makeBoolBytesTuple(true,
		ApplicationCall::setReturnData(_ctx.typeMapper, std::move(result), _loc, _ctx.preEffects()), _loc));
}

} // namespace puyasol::builder::eb
