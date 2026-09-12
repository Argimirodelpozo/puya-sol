/// @file InnerCallShapes.cpp
/// Per-shape handlers for address.call(...):
///   - handleCallWithEncodeCall   (typed abi.encodeCall self/cross calls)
///   - handleCallWithRawData      (low-level rawBytes)
///   - handleStaticCallPrecompile (0x01..0x0a precompile routing)

#include "builder/itxn/InnerCallHandlers.h"
#include "builder/itxn/NativePayment.h"
#include "builder/itxn/ApplicationCall.h"
#include "builder/sol-ast/CallOperands.h"
#include "builder/AwstShorthand.h"
#include "builder/EvmFeaturePolicy.h"
#include "builder/SolcFacts.h"
#include "awst/NameGen.h"
#include "builder/itxn/InnerCallInternal.h"
#include "builder/itxn/CallResolver.h"
#include "builder/sol-eb/SolBoolBuilder.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-types/TypeMapper.h"
#include "Logger.h"
#include "builder/itxn/Precompile.h"

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/TypeProvider.h>

namespace puyasol::builder::eb
{

std::unique_ptr<InstanceBuilder> InnerCallHandlers::handleCallWithEncodeCall(
	ContractContext& _ctx,
	std::shared_ptr<awst::Expression> _receiver,
	solidity::frontend::FunctionCall const& _encodeCallExpr,
	std::shared_ptr<awst::Expression> _callValue,
	awst::SourceLocation const& _loc)
{
	using namespace solidity::frontend;

	if (_encodeCallExpr.arguments().size() < 2)
		return nullptr;

	auto const& targetFnExpr = *_encodeCallExpr.arguments()[0];
	FunctionDefinition const* targetFuncDef = nullptr;

	auto const* targetFnType = dynamic_cast<FunctionType const*>(
		targetFnExpr.annotation().type);
	if (targetFnType && targetFnType->hasDeclaration())
		targetFuncDef = dynamic_cast<FunctionDefinition const*>(
			&targetFnType->declaration());

	if (!targetFuncDef)
		return nullptr;
	if (shorthand::isCurrentAppAddressGlobal(_receiver.get()))
	{
		assert(!_callValue); // rejected by the dispatcher before self lowering
		auto form = parseSelfEncodeForm(_encodeCallExpr,
			dynamic_cast<MemberAccess const*>(&_encodeCallExpr.expression()));
		return emitDirectSelfCall(_ctx, *targetFuncDef, form, "encodeCall", _loc);
	}
	_ctx.evaluateForEffects(targetFnExpr, _loc);

	auto const& argsExpr = *_encodeCallExpr.arguments()[1];
	std::vector<ASTPointer<Expression const>> callArgs;
	if (auto const* tupleExpr = dynamic_cast<TupleExpression const*>(&argsExpr))
	{
		for (auto const& comp : tupleExpr->components())
			if (comp) callArgs.push_back(comp);
	}
	else
		callArgs.push_back(_encodeCallExpr.arguments()[1]);

	std::vector<Type const*> paramTypes;
	for (auto const& parameter: targetFuncDef->parameters())
		paramTypes.push_back(parameter->type());
	// abi.encodeCall carries Solidity ABI bytes in every transport profile.
	// ARC4 contracts expose the EVM compatibility route as well, including
	// its canonical return encoding (a uint64 still occupies a 32-byte word).
	auto selector = awst::makeBytesConstant(
		builder::SolcFacts::externalSelector(*targetFnType), _loc,
		awst::BytesEncoding::Base16, awst::WType::bytesType());
	return submitTypedAppCall(_ctx, std::move(_receiver),
		buildEvmApplicationArgs(_ctx, std::move(selector), callArgs, paramTypes, _loc),
		std::move(_callValue), _loc);
}

// ── Shared tail: typed inner app call ──
// ApplicationArgs[0] = 4-byte selector, [1..n] = ARC4-encoded args.
// Returndata = LastLog[4:] (strip 0x151f7c75 prefix; see EVM_DIVERGENCE).
// _callValue != nullptr → [PaymentTxn, ApplicationCall] group: the payment
// precedes the app call, so the callee's msg.value (gtxns Amount at
// GroupIndex-1) and its non-payable check both see it.
std::unique_ptr<InstanceBuilder> InnerCallHandlers::submitTypedAppCall(
	ContractContext& _ctx,
	std::shared_ptr<awst::Expression> _receiver,
	std::shared_ptr<awst::TupleExpression> _argsTuple,
	std::shared_ptr<awst::Expression> _callValue,
	awst::SourceLocation const& _loc)
{
	EvmFeaturePolicy::report(
		EvmFeature::LowLevelCallOutcome, _ctx.typeMapper.profile(), _loc);
	std::vector<awst::WType const*> argTypes;
	for (auto const& item : _argsTuple->items)
		argTypes.push_back(item->wtype);
	_argsTuple->wtype = _ctx.typeMapper.createType<awst::WTuple>(std::move(argTypes), std::nullopt);

	return submitAppCall(_ctx, std::move(_receiver), std::move(_argsTuple), std::move(_callValue), _loc);
}

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

// ── .call(abi.encodeWithSignature/WithSelector(...)) → typed inner call ──
// Encoder is visible: each arg goes into its own ApplicationArg (ARC4).
// Self-receiver literal-sig is already rewritten by the dispatcher;
// anything else reaching here with self receiver fails at runtime (AVM rejects self txns).
std::unique_ptr<InstanceBuilder> InnerCallHandlers::handleCallWithSignatureArgs(
	ContractContext& _ctx,
	std::shared_ptr<awst::Expression> _receiver,
	solidity::frontend::FunctionCall const& _encodeExpr,
	bool _isSignature,
	std::shared_ptr<awst::Expression> _callValue,
	awst::SourceLocation const& _loc)
{
	using namespace solidity::frontend;
	auto const& args = _encodeExpr.arguments();
	if (args.empty())
		return nullptr;
	// abi.encodeWithSignature fixes its payload to keccak256(sig)[:4] ++
	// EVM-encoded args in EVERY profile (entry transport must not change the
	// meaning of an abi.* expression), so the recognized fast path forwards
	// exactly that shape — identical bytes to the unrecognized payload going
	// through handleCallWithRawData's [first4, rest] split. An ARC-4-profile
	// callee dispatches it through its EVM compat arms. The old profile fork
	// re-derived sha512_256(signature-as-given) here, which matches NOTHING:
	// not the ARC-4 route (that hashes name(args)return) and not keccak — every
	// `.call(abi.encodeWithSignature(...))` erred inside the callee's router.
	if (_isSignature || _ctx.typeMapper.profile().contractAbi == ContractAbi::Evm)
	{
		std::shared_ptr<awst::Expression> evmSelector;
		if (_isSignature)
		{
			if (auto const* literal = dynamic_cast<Literal const*>(args[0].get()))
				evmSelector = awst::makeBytesConstant(
					builder::SolcFacts::externalSelector(literal->value()), _loc,
					awst::BytesEncoding::Base16, awst::WType::bytesType());
			else
			{
				auto hash = awst::makeIntrinsicCall(
					"keccak256", awst::WType::bytesType(), _loc);
				hash->stackArgs.push_back(
					awst::makeAsBytes(sol_ast::CallOperands::evaluate(_ctx, *args[0], _loc), _loc));
				evmSelector = awst::makeExtract(std::move(hash), 0, 4, _loc);
			}
		}
		else
		{
			// encodeWithSelector's first arg types as bytes4, but a NUMBER
			// literal (abi.encodeWithSelector(0xdeadbeef)) builds as
			// uint64/biguint — asBytes on those is an invalid cast, and itob
			// alone would yield 8 bytes. Take the low-order 4 bytes, matching
			// handleEncodeWithSelector's coercion.
			auto sel = sol_ast::CallOperands::evaluate(_ctx, *args[0], _loc);
			if (sel->wtype == awst::WType::uint64Type())
				sel = awst::makeExtractLastN(
					awst::makeItob(std::move(sel), _loc), 4, _loc);
			else if (sel->wtype == awst::WType::biguintType())
				sel = awst::makeExtractLastN(
					awst::makeLeftPad(
						awst::makeAsBytes(std::move(sel), _loc), 4, _loc),
					4, _loc);
			else
				sel = awst::makeAsBytes(std::move(sel), _loc);
			evmSelector = std::move(sel);
		}

		std::vector<ASTPointer<Expression const>> callArgs;
		for (size_t i = 1; i < args.size(); ++i)
			callArgs.push_back(args[i]);
		return submitTypedAppCall(_ctx, std::move(_receiver),
			buildEvmApplicationArgs(_ctx, std::move(evmSelector), callArgs,
				{}, _loc),
			std::move(_callValue), _loc);
	}

	std::shared_ptr<awst::Expression> selector;
	{
		// Explicit Solidity→ARC-4 transport boundary. In --evm-selectors mode
		// `C.f.selector` is keccak-derived and must not be forwarded directly to
		// the AVM router. When the selector expression names a declaration, retain
		// the router identity here; genuinely opaque runtime selectors remain
		// untranslatable without the target ABI.
		FunctionDefinition const* target = nullptr;
		VariableDeclaration const* getter = nullptr;
		FunctionType const* targetType = nullptr;
		if (auto const* selectorAccess =
				dynamic_cast<MemberAccess const*>(args[0].get()))
			if (selectorAccess->memberName() == "selector")
			{
				auto const& functionExpr = selectorAccess->expression();
				targetType = dynamic_cast<FunctionType const*>(
					functionExpr.annotation().type);
				if (targetType && targetType->hasDeclaration())
				{
					target = dynamic_cast<FunctionDefinition const*>(
						&targetType->declaration());
					getter = dynamic_cast<VariableDeclaration const*>(
						&targetType->declaration());
				}
				if (!target)
					target = dynamic_cast<FunctionDefinition const*>(
						ASTNode::referencedDeclaration(functionExpr));
			}
		if (target)
		{
			// Even when the declaration lets us substitute the ARC-4 selector at
			// compile time, Solidity still evaluates the selector expression (for
			// example `(sideEffect(), C.f).selector`).
			_ctx.evaluateForEffects(*args[0], _loc);
			selector = awst::makeMethodConstant(
				buildMethodSelector(_ctx, target), awst::WType::bytesType(), _loc);
		}
		else if (getter && targetType)
		{
			_ctx.evaluateForEffects(*args[0], _loc);
			selector = awst::makeMethodConstant(
				buildMethodSelector(_ctx, getter->name(), *targetType),
				awst::WType::bytesType(), _loc);
		}
		else
			selector = awst::makeAsBytes(sol_ast::CallOperands::evaluate(_ctx, *args[0], _loc), _loc);
	}

	auto argsTuple = awst::makeTupleExpression(nullptr, _loc);
	argsTuple->items.push_back(std::move(selector));
	std::vector<ASTPointer<Expression const>> sourceArgs(args.begin() + 1, args.end());
	auto values = lowerArguments(_ctx, sourceArgs, {}, _loc);
	for (size_t i = 1; i < args.size(); ++i)
		// encodeWithSelector/Signature is TYPE-LESS (no declared params); the shared
		// encoder's nullptr path keeps backing-width encoding (biguint→32B, bare itob).
		argsTuple->items.push_back(
			encodeArgToBytes(_ctx, std::move(values[i - 1]),
				args[i]->annotation().type, nullptr, _loc));

	return submitTypedAppCall(_ctx, std::move(_receiver), std::move(argsTuple), std::move(_callValue), _loc);
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
