/// @file InnerCallHandlers.cpp
/// Handles address.call/staticcall/delegatecall/transfer inner transaction patterns
/// and precompile routing.

#include "builder/lowering/itxn/InnerCallHandlers.h"
#include "builder/solc/SolcFacts.h"
#include "builder/solc/SolcConstFold.h"
#include "builder/codec/EvmValueCodec.h"
#include "builder/codec/EvmAbiDecode.h"
#include "builder/context/BuildArtifacts.h"
#include "builder/eb/CallOperands.h"
#include "builder/AwstShorthand.h"
#include "awst/NameGen.h"
#include "builder/target/EvmFeaturePolicy.h"
#include "builder/lowering/abi/AbiEncoderBuilder.h"
#include "builder/lowering/abi/AbiSelectorCalldataBuilder.h"
#include "builder/types/SolIntType.h"
#include "builder/storage/StateVarWalker.h"
#include "builder/lowering/itxn/InnerCallInternal.h"
#include "builder/lowering/calls/CallResolver.h"
#include "builder/eb/SolBoolBuilder.h"
#include "builder/storage/slot/EvmSlotLowering.h"
#include "builder/types/ConversionPlan.h"
#include "builder/types/TypeCoercion.h"
#include "builder/types/TypeMapper.h"
#include "builder/lowering/itxn/NativePayment.h"
#include "builder/lowering/itxn/ApplicationCall.h"
#include "Logger.h"

namespace puyasol::builder::eb
{

// ── Helpers ──

std::optional<uint64_t> detectPrecompileAddress(
	solidity::frontend::Expression const& expression)
{
	auto address = SolcConstFold::constantAddress(expression);
	return address && *address >= 1 && *address <= 10
		? std::optional<uint64_t>(static_cast<uint64_t>(*address)) : std::nullopt;
}

static awst::WTuple s_boolBytesType(
	std::vector<awst::WType const*>{awst::WType::boolType(), awst::WType::bytesType()});

std::shared_ptr<awst::Expression> InnerCallHandlers::makeBoolBytesTuple(
	bool _success,
	std::shared_ptr<awst::Expression> _data,
	awst::SourceLocation const& _loc)
{
	auto tuple = awst::makeTupleExpression(&s_boolBytesType, _loc);
	tuple->items.push_back(awst::makeBoolConstant(_success, _loc));
	tuple->items.push_back(std::move(_data));
	return tuple;
}

std::shared_ptr<awst::Expression> InnerCallHandlers::makeBoolBytesTupleEmpty(
	awst::SourceLocation const& _loc)
{
	return makeBoolBytesTuple(true, awst::makeBytesConstant({}, _loc), _loc);
}

std::vector<std::shared_ptr<awst::Expression>> InnerCallHandlers::lowerArguments(
	ContractContext& _ctx,
	std::vector<solidity::frontend::ASTPointer<
		solidity::frontend::Expression const>> const& _args,
	std::vector<solidity::frontend::Type const*> const& _paramTypes,
	awst::SourceLocation const& _loc, bool reinterpret)
{
	std::vector<std::shared_ptr<awst::Expression>> values;
	for (size_t i = 0; i < _args.size(); ++i)
	{
		values.push_back(sol_ast::CallOperands::evaluate(_ctx, *_args[i], _loc, [&] {
			auto value = _ctx.buildExpr(*_args[i]);
			if (i < _paramTypes.size() && _paramTypes[i])
			{
				auto const* source = _args[i]->annotation().type;
				auto const* native = _ctx.typeMapper.map(_paramTypes[i]);
				value = sol_ast::EvmSlotLowering::materializeRefValue(
					_ctx, std::move(value), source, native, _loc);
				value = ConversionPlan{source, _paramTypes[i], native,
					reinterpret ? ConversionPlan::Context::AbiReinterpret
						: ConversionPlan::Context::AbiArgument}.emit(std::move(value), _loc);
			}
			return value;
		}));
	}
	return values;
}

std::string solTypeToArc4ParamName(
	ContractContext& _ctx, solidity::frontend::Type const* _type)
{
	CallParameterPlan parameter;
	parameter.type = _ctx.typeMapper.map(_type);
	parameter.setAbiWireType(_ctx.typeMapper, _type);
	return TypeCoercion::wtypeToABIName(parameter.wireType);
}

std::string solTypeToArc4ReturnName(
	ContractContext& _ctx, solidity::frontend::Type const* _type)
{
	auto const plan = planReturnElement(_ctx.typeMapper, _type,
		abiReturnNativeType(_ctx.typeMapper, _type));
	return TypeCoercion::wtypeToABIName(plan.wireType);
}

std::string InnerCallHandlers::buildMethodSelector(
	ContractContext& _ctx,
	std::string const& _name,
	solidity::frontend::FunctionType const& _funcType)
{
	std::vector<std::string> paramNames, retNames;
	for (auto const& paramType : _funcType.parameterTypes())
		paramNames.push_back(solTypeToArc4ParamName(_ctx, paramType));
	for (auto const& retType : _funcType.returnParameterTypes())
		retNames.push_back(solTypeToArc4ReturnName(_ctx, retType));
	return builder::TypeCoercion::buildArc4Selector(_name, paramNames, retNames);
}

std::string InnerCallHandlers::buildMethodSelector(
	ContractContext& _ctx,
	solidity::frontend::FunctionDefinition const* _func)
{
	std::vector<std::string> paramNames, retNames;
	for (auto const& parameter: _ctx.typeMapper.callBoundaryPlan(*_func).parameters)
		paramNames.push_back(TypeCoercion::wtypeToABIName(parameter.wireType));
	for (auto const& element: _ctx.typeMapper.functionReturnPlan(*_func).elements)
		retNames.push_back(TypeCoercion::wtypeToABIName(element.wireType));
	return builder::TypeCoercion::buildArc4Selector(_func->name(), paramNames, retNames);
}

// ── Payment ──

std::unique_ptr<InstanceBuilder> InnerCallHandlers::handleTransfer(
	ContractContext& _ctx, std::shared_ptr<awst::Expression> _receiver,
	std::shared_ptr<awst::Expression> _amount, awst::SourceLocation const& _loc)
{
	_ctx.postEffects().push_back(buildNativeTransfer(_ctx.typeMapper, _ctx.preEffects(),
		std::move(_receiver), std::move(_amount), _loc));

	auto vc = awst::makeVoidConstant(_loc);
	return std::make_unique<GenericResultBuilder>(_ctx, std::move(vc));
}

std::unique_ptr<InstanceBuilder> InnerCallHandlers::handleSend(
	ContractContext& _ctx, std::shared_ptr<awst::Expression> _receiver,
	std::shared_ptr<awst::Expression> _amount, awst::SourceLocation const& _loc)
{
	EvmFeaturePolicy::report(
		EvmFeature::LowLevelCallOutcome, _ctx.typeMapper.profile(), _loc);
	_ctx.postEffects().push_back(buildNativeTransfer(_ctx.typeMapper, _ctx.preEffects(),
		std::move(_receiver), std::move(_amount), _loc));

	return std::make_unique<SolBoolBuilder>(_ctx, awst::makeTrue(_loc));
}

std::unique_ptr<InstanceBuilder> InnerCallHandlers::handleCallWithValue(
	ContractContext& _ctx, std::shared_ptr<awst::Expression> _receiver,
	std::shared_ptr<awst::Expression> _amount, awst::SourceLocation const& _loc)
{
	EvmFeaturePolicy::report(
		EvmFeature::LowLevelCallOutcome, _ctx.typeMapper.profile(), _loc);
	_ctx.preEffects().push_back(buildNativeTransfer(_ctx.typeMapper, _ctx.preEffects(),
		std::move(_receiver), std::move(_amount), _loc));
	return std::make_unique<GenericResultBuilder>(_ctx, makeBoolBytesTuple(true,
		_ctx.emitSequencedOperand({}, ApplicationCall::returnData(_ctx.typeMapper, _loc), true, _loc), _loc));
}

// ── .call(abi.encodeCall(...)) ──

std::unique_ptr<InstanceBuilder> InnerCallHandlers::handleDelegatecall(
	ContractContext& _ctx,
	solidity::frontend::FunctionCall const& _callNode,
	awst::SourceLocation const& _loc)
{
	// AVM has no DELEGATECALL: every app has isolated storage, every inner txn
	// has its own caller. Fail LOUDLY — but at RUNTIME, on reach: a compile
	// error here rejected whole real-world trees over OZ Address.sol's
	// unreachable functionDelegateCall utility (Aave's Pool never calls it;
	// puya's DCE strips the unreached body entirely). Any delegatecall that
	// actually EXECUTES still dies with an explicit message, never a silently
	// wrong stub.
	EvmFeaturePolicy::report(
		EvmFeature::DelegateCall, _ctx.typeMapper.profile(), _loc);
	for (auto const& arg : _callNode.arguments())
		_ctx.evaluateForEffects(*arg, _loc);
	// assert(false) is a compile-time TERMINATOR: puya flags the statements
	// that consume the (bool, bytes) result as unreachable and rejects the
	// whole program (fbtc). Use a runtime-opaque always-false condition —
	// Global.Round is never 0 on any chain, and puya cannot fold it.
	auto round = awst::makeIntrinsicCall(
		"global", awst::WType::uint64Type(), _loc);
	round->immediates.push_back("Round");
	auto neverTrue = awst::makeNumericCompare(std::move(round),
		awst::NumericComparison::Eq,
		awst::makeIntegerConstant(uint64_t{0}, _loc), _loc);
	_ctx.queuePreExpression(awst::makeAssert(std::move(neverTrue), _loc,
		"delegatecall is not supported on AVM"), _loc);
	return std::make_unique<GenericResultBuilder>(_ctx, makeBoolBytesTupleEmpty(_loc));
}

// ── Top-level dispatcher ──

namespace
{
/// Receiver lowers to `global CurrentApplicationAddress` — a self-call.
constexpr auto isCurrentAppAddressReceiver = shorthand::isCurrentAppAddressGlobal;
} // anonymous namespace

InnerCallHandlers::SelfEncodeForm InnerCallHandlers::parseSelfEncodeForm(
	solidity::frontend::FunctionCall const& encCall,
	solidity::frontend::MemberAccess const* encMA)
{
	using namespace solidity::frontend;
	auto const* encCallExpr = &encCall;
	SelfEncodeForm form;
	auto& sigString = form.sigString;
	auto& refFunc = form.refFunc;
	auto& targetIdentityExpr = form.targetIdentityExpr;
	auto& resolvedArgs = form.resolvedArgs;
	// Both recognised shapes lower to a direct InstanceMethodTarget:
	//   address(this).call(abi.encodeWithSignature("fn(types)", args...))
	//   address(this).call(abi.encodeWithSelector(this.fn.selector, args...))
	// Arg normalisation across the three encode forms:
	//   encodeWithSignature("fn(types)", a, b, …) → args spread at indices 1..
	//   encodeWithSelector(this.fn.selector, a, b, …) → args spread at indices 1..
	//   encodeCall(C.fn, (a, b, …)) → args as a TUPLE in index 1 (or a single value)
	if (encMA && encMA->memberName() == "encodeWithSignature"
		&& !encCallExpr->arguments().empty())
	{
		if (auto const* sigLit = SolcFacts::expressionAs<Literal>(encCallExpr->arguments()[0].get()))
		{
			sigString = sigLit->value();
		}
		for (size_t i = 1; i < encCallExpr->arguments().size(); ++i)
			resolvedArgs.push_back(encCallExpr->arguments()[i]);
	}
	else if (encMA && encMA->memberName() == "encodeWithSelector"
		&& !encCallExpr->arguments().empty())
	{
		targetIdentityExpr = encCallExpr->arguments()[0].get();
		// `this.fn.selector` = MemberAccess("selector", MemberAccess("fn", this)).
		if (auto const* selMA = SolcFacts::expressionAs<MemberAccess>(encCallExpr->arguments()[0].get()))
			if (selMA->memberName() == "selector")
				if (auto const* fnMA = SolcFacts::expressionAs<MemberAccess>(&selMA->expression()))
				{
					refFunc = dynamic_cast<FunctionDefinition const*>(
						fnMA->annotation().referencedDeclaration);
				}
		for (size_t i = 1; i < encCallExpr->arguments().size(); ++i)
			resolvedArgs.push_back(encCallExpr->arguments()[i]);
	}
	else if (encMA && encMA->memberName() == "encodeCall"
		&& !encCallExpr->arguments().empty())
	{
		AbiCall facts(encCall);
		targetIdentityExpr = facts.target;
		auto const* type = dynamic_cast<FunctionType const*>(facts.target->annotation().type);
		if (type && type->hasDeclaration())
			refFunc = dynamic_cast<FunctionDefinition const*>(&type->declaration());
		resolvedArgs = std::move(facts.arguments);
	}
	return form;
}

/// Match Solidity's exact external signature, never an ARC4 carrier or name/arity.
solidity::frontend::FunctionDefinition const* InnerCallHandlers::resolveSelfCallOverload(
	ContractContext& _ctx,
	SelfEncodeForm const& form)
{
	using namespace solidity::frontend;
	if (!_ctx.currentContract) return nullptr;
	auto signature = form.refFunc ? form.refFunc->externalSignature() : form.sigString;
	if (signature.empty()) return nullptr;
	for (auto const& [_, type]: _ctx.currentContract->interfaceFunctionList(true))
		if (type && type->externalSignature() == signature && type->hasDeclaration()
			&& type->parameterTypes().size() == form.resolvedArgs.size())
			if (auto const* function = dynamic_cast<FunctionDefinition const*>(&type->declaration());
				function && function->isImplemented())
				return function;
	return nullptr;
}

/// Emit the direct-callsub rewrite for a resolved self-call target and wrap the result as the EVM `(bool, bytes)` tuple.
std::unique_ptr<InstanceBuilder> InnerCallHandlers::emitDirectSelfCall(
	ContractContext& _ctx,
	solidity::frontend::FunctionDefinition const& targetFunc,
	SelfEncodeForm const& form,
	std::string const& encodeName,
	bool staticCall,
	awst::SourceLocation const& _loc)
{
	using namespace solidity::frontend;
	auto const* target = &targetFunc;
	auto const* targetIdentityExpr = form.targetIdentityExpr;
	auto const& resolvedArgs = form.resolvedArgs;
	if (targetIdentityExpr)
		_ctx.evaluateForEffects(*targetIdentityExpr, _loc);
	// AVM rejects self inner-txn calls; rewrite to direct callsub.
	// Revert isolation differs: reverts propagate instead of
	// being caught as success=false.
	EvmFeaturePolicy::report(
		EvmFeature::SelfCall, _ctx.typeMapper.profile(), _loc);

	std::string targetName =
		CallResolver::resolveMethodName(_ctx, *target);
	auto const& signature = _ctx.typeMapper.functionReturnPlan(*target);
	auto const& boundary = _ctx.typeMapper.callBoundaryPlan(*target, _ctx.currentContract);
	auto call = awst::makeSubroutineCall(
		awst::InstanceMethodTarget{targetName}, signature.wireType, _loc);
	std::vector<Type const*> paramTypes, returnTypes;
	for (auto const& parameter: target->parameters()) paramTypes.push_back(parameter->type());
	for (auto const& result: target->returnParameters()) returnTypes.push_back(result->type());
	auto values = lowerArguments(_ctx, resolvedArgs, paramTypes, _loc, encodeName != "encodeCall");
	for (size_t i = 0; i < values.size(); ++i)
		awst::pushCallArg(call->args, boundary.parameters[i].wireName(),
			boundary.parameters[i].encodeArgument(std::move(values[i]), _loc));
	auto value = ApplicationCall::withStaticContext(_ctx.typeMapper, std::move(call),
		staticCall, _loc, _ctx.preEffects());
	auto bytes = ApplicationCall::setTypedReturnData(_ctx.typeMapper, std::move(value),
		returnTypes, true, _loc, _ctx.preEffects());
	return std::make_unique<GenericResultBuilder>(_ctx, makeBoolBytesTuple(true, std::move(bytes), _loc));
}

/// `.call/.staticcall(data)` with a data argument — the encoded-call router: self-call direct rewrites …
std::unique_ptr<InstanceBuilder> InnerCallHandlers::handleCallWithData(
	ContractContext& _ctx,
	std::shared_ptr<awst::Expression> _receiver,
	std::string const& _memberName,
	solidity::frontend::FunctionCall const& _callNode,
	std::shared_ptr<awst::Expression> _callValue,
	solidity::frontend::Expression const& _baseExpr,
	awst::SourceLocation const& _loc)
{
	using namespace solidity::frontend;

	auto const& dataArg = SolcFacts::unparenthesized(*_callNode.arguments()[0]);

	// {value:} needs an inner PaymentTxn grouped with a real inner app call.
	// Self-calls rewrite to a direct callsub (no inner txn to attach it to)
	// and precompiles have no account to pay — fail loud, don't drop value.
	if (_callValue)
	{
		bool selfReceiver = isCurrentAppAddressReceiver(_receiver.get());
		if (selfReceiver || detectPrecompileAddress(_baseExpr))
		{
			Logger::instance().error(
				std::string("`.call{value: ...}(data)` to ")
					+ (selfReceiver ? "the contract itself" : "a precompile address")
					+ " is not supported on AVM: the value payment cannot be "
					  "attached (self-calls lower to a direct subroutine call; "
					  "precompiles have no account). Split into a separate "
					  "transfer + call.", _loc);
			_ctx.evaluateForEffects(dataArg, _loc);
			return std::make_unique<GenericResultBuilder>(_ctx, makeBoolBytesTupleEmpty(_loc));
		}
	}

	// .call(data) to known precompile address → route like .staticcall
	if (auto precompileAddr = detectPrecompileAddress(_baseExpr))
	{
		auto inputData = sol_ast::CallOperands::evaluate(_ctx, dataArg, _loc);
		auto result = handleStaticCallPrecompile(_ctx, *precompileAddr, std::move(inputData), _loc);
		if (result) return result;
	}
	// Proven builtin self calls retain their direct-call optimization.
	if (isCurrentAppAddressReceiver(_receiver.get()))
	{
		if (auto const* encCallExpr = SolcFacts::expressionAs<FunctionCall>(&dataArg))
		{
			auto const* encMA = SolcFacts::expressionAs<MemberAccess>(&SolcFacts::functionExpression(encCallExpr->expression()));
			auto const* type = dynamic_cast<FunctionType const*>(encCallExpr->expression().annotation().type);
			bool recognised = encMA && type && !encCallExpr->arguments().empty()
				&& (type->kind() == FunctionType::Kind::ABIEncodeWithSignature
					|| type->kind() == FunctionType::Kind::ABIEncodeWithSelector
					|| type->kind() == FunctionType::Kind::ABIEncodeCall);
			if (recognised)
			{
				auto form = parseSelfEncodeForm(*encCallExpr, encMA);
				if (auto const* target = resolveSelfCallOverload(_ctx, form))
					return emitDirectSelfCall(
						_ctx, *target, form, encMA->memberName(), _memberName == "staticcall", _loc);
			}
		}
	}

	if (isCurrentAppAddressReceiver(_receiver.get()))
	{
		EvmFeaturePolicy::report(EvmFeature::SelfCall, _ctx.typeMapper.profile(), _loc);
		if (!_ctx.currentContract)
			throw std::logic_error("Self-call dispatch requires a concrete host contract");
		_ctx.typeMapper.artifacts().contract().needsSelfCallDispatch = true;
		auto call = awst::makeSubroutineCall(
			awst::InstanceMethodTarget{"__puyasol_self_call"}, &s_boolBytesType, _loc);
		awst::pushCallArg(call->args, awst::makeAsBytes(
			sol_ast::CallOperands::evaluate(_ctx, dataArg, _loc), _loc));
		return std::make_unique<GenericResultBuilder>(_ctx, ApplicationCall::withStaticContext(
			_ctx.typeMapper, std::move(call), _memberName == "staticcall", _loc, _ctx.preEffects()));
	}

	auto dataOperand = _ctx.lower(dataArg, false);
	auto dataExpr = _ctx.emitSequencedOperand(
		std::move(dataOperand.effects), std::move(dataOperand.value), false, _loc);
	// ARC4's explicit selector boundary is retained only when solc identifies
	// the declaration. Decode the actual EVM argument body before converting
	// to native ApplicationArgs: never guess carrier widths from source syntax.
	auto const* encoder = SolcFacts::expressionAs<FunctionCall>(&dataArg);
	auto const* builtin = encoder ? dynamic_cast<FunctionType const*>(encoder->expression().annotation().type) : nullptr;
	auto const* selector = builtin && builtin->kind() == FunctionType::Kind::ABIEncodeWithSelector
		? SolcFacts::expressionAs<MemberAccess>(encoder->arguments()[0].get()) : nullptr;
	auto const* function = selector && selector->memberName() == "selector"
		? dynamic_cast<FunctionType const*>(selector->expression().annotation().type) : nullptr;
	if (_ctx.typeMapper.profile().contractAbi == ContractAbi::Arc4 && function && function->hasDeclaration())
	{
		auto const& parameters = function->parameterTypes();
		std::vector<awst::WType const*> nativeTypes;
		for (auto const* parameter: parameters) nativeTypes.push_back(_ctx.typeMapper.map(parameter));
		auto const& declaration = function->declaration();
		auto const* definition = dynamic_cast<FunctionDefinition const*>(&declaration);
		auto selector = awst::makeMethodConstant(definition
			? buildMethodSelector(_ctx, definition)
			: buildMethodSelector(_ctx, declaration.name(), *function), awst::WType::bytesType(), _loc);
		std::vector<std::shared_ptr<awst::Expression>> values;
		if (!parameters.empty())
		{
			auto const* decodedType = parameters.size() == 1 ? nativeTypes[0]
				: _ctx.typeMapper.createType<awst::WTuple>(nativeTypes);
			auto decoded = awst::makeEvalOnce(abi::decodeEvmAbi(_ctx.typeMapper,
				awst::makeExtract(dataExpr, 4, 0, _loc), parameters, decodedType, _loc, _ctx.preEffects()), _loc);
			for (size_t i = 0; i < parameters.size(); ++i)
				values.push_back(parameters.size() == 1 ? decoded
					: awst::makeTupleItem(decoded, static_cast<int>(i), nativeTypes[i], _loc));
		}
		auto args = ApplicationCall::encodeArguments(_ctx.typeMapper, std::move(selector),
			parameters, std::move(values), _loc, _ctx.preEffects());
		EvmFeaturePolicy::report(EvmFeature::LowLevelCallOutcome, _ctx.typeMapper.profile(), _loc);
		return submitAppCall(_ctx, std::move(_receiver), std::move(args), std::move(_callValue), _loc);
	}
	auto isEmptyConst = [](awst::Expression const* e) {
		// Unwrap ReinterpretCast (string→bytes, etc.) to inspect the inner.
		while (auto const* rc = dynamic_cast<awst::ReinterpretCast const*>(e))
			e = rc->expr.get();
		if (auto const* bc = dynamic_cast<awst::BytesConstant const*>(e))
			return bc->value.empty();
		if (auto const* sc = dynamic_cast<awst::StringConstant const*>(e))
			return sc->value.empty();
		return false;
	};
	if (isEmptyConst(dataExpr.get()))
	{
		// Empty data still executes an application's receive/fallback.
		if (_callValue)
			return handleCallWithValue(_ctx, std::move(_receiver), std::move(_callValue), _loc);
		if (SolcConstFold::constantAddress(_baseExpr) == std::optional<solidity::u256>(0))
			return std::make_unique<GenericResultBuilder>(_ctx,
				makeBoolBytesTuple(true, ApplicationCall::setReturnData(_ctx.typeMapper,
					awst::makeBytesConstant({}, _loc), _loc, _ctx.preEffects()), _loc));
		// Zero-value empty call: solc EXECUTES the callee (receive, or
		// fallback when no receive) — zero-arg inner app call.
		return handleCallWithEmptyData(_ctx, std::move(_receiver), _loc);
	}
	return handleCallWithRawData(_ctx, _receiver, std::move(dataExpr), std::move(_callValue), _loc);
}

std::unique_ptr<InstanceBuilder> InnerCallHandlers::tryHandleAddressCall(
	ContractContext& _ctx,
	std::shared_ptr<awst::Expression> _receiver,
	std::string const& _memberName,
	solidity::frontend::FunctionCall const& _callNode,
	std::shared_ptr<awst::Expression> _callValue,
	solidity::frontend::Expression const& _baseExpr,
	awst::SourceLocation const& _loc)
{
	using namespace solidity::frontend;

	// .transfer(amount)
	if (_memberName == "transfer" && _callNode.arguments().size() == 1)
	{
		auto amount = sol_ast::CallOperands::evaluate(_ctx, *_callNode.arguments()[0], _loc);
		return handleTransfer(_ctx, std::move(_receiver), std::move(amount), _loc);
	}

	// .send(amount)
	if (_memberName == "send" && _callNode.arguments().size() == 1)
	{
		auto amount = sol_ast::CallOperands::evaluate(_ctx, *_callNode.arguments()[0], _loc);
		return handleSend(_ctx, std::move(_receiver), std::move(amount), _loc);
	}

	// .call{value: X} with NO data argument → bare payment. A non-empty data
	// argument must ALSO invoke the target (payment + app call in one inner
	// group) — matching any .call{value:} here silently dropped the calldata.
	if (_memberName == "call" && _callValue && _callNode.arguments().empty())
		return handleCallWithValue(_ctx, std::move(_receiver), std::move(_callValue), _loc);

	// .call(abi.encodeCall(...)) → inner app call
	// staticcall lowers IDENTICALLY to call on the AVM (an inner ApplicationCall txn): there is no
	// inner-txn read-only flag, so the EVM static (no-state-change) guarantee can't be enforced. Route
	// staticcall through the same handling as call and warn that "static" is not respected. (Precompile
	// staticcalls, self-calls, encodeCall/encodeWithSignature typed routing all come along for free.)
	// solc rejects {value:} on staticcall, so _callValue here implies _memberName == "call".
	if ((_memberName == "call" || _memberName == "staticcall") && !_callNode.arguments().empty())
		return handleCallWithData(
			_ctx, std::move(_receiver), _memberName, _callNode,
			std::move(_callValue), _baseExpr, _loc);

	if (_memberName == "delegatecall")
		return handleDelegatecall(_ctx, _callNode, _loc);

	return nullptr;
}

} // namespace puyasol::builder::eb
