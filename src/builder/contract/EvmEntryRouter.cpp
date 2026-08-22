#include "builder/contract/ContractBuilder.h"

#include "builder/contract/RouterConditions.h"

#include "Logger.h"
#include "builder/SolcFacts.h"
#include "builder/abi/EvmAbiDecode.h"
#include "builder/abi/EvmAbiEncode.h"
#include "builder/codec/EvmValueCodec.h"
#include "builder/sol-types/OverloadSuffix.h"
#include "builder/sol-types/TypeMapper.h"

#include <libsolidity/ast/AST.h>

#include <functional>

namespace puyasol::builder
{
using namespace solidity::frontend;

namespace
{
std::shared_ptr<awst::Expression> u64(
	uint64_t value, awst::SourceLocation const& loc)
{
	return awst::makeIntegerConstant(value, loc);
}

std::string methodNameFor(
	FunctionType const& function, OverloadedNamesSet const& overloaded)
{
	if (!function.hasDeclaration())
		return {};
	if (auto const* definition =
			dynamic_cast<FunctionDefinition const*>(&function.declaration()))
	{
		std::string name = definition->name();
		if (overloaded.count(name))
			appendOverloadSuffix(name, *definition);
		return name;
	}
	if (auto const* variable =
			dynamic_cast<VariableDeclaration const*>(&function.declaration()))
		return variable->name();
	return {};
}

awst::ContractMethod* findMethod(awst::Contract& contract, std::string const& name)
{
	for (auto& method: contract.methods)
		if (method.memberName == name)
			return &method;
	return nullptr;
}

void emitReturnLog(
	std::shared_ptr<awst::Expression> payload,
	awst::SourceLocation const& loc,
	std::vector<std::shared_ptr<awst::Statement>>& out)
{
	auto log = awst::makeIntrinsicCall("log", awst::WType::voidType(), loc);
	log->stackArgs.push_back(awst::makeConcat(
		awst::makeBytesConstant({0x15, 0x1f, 0x7c, 0x75}, loc),
		std::move(payload), loc));
	out.push_back(awst::makeExpressionStatement(std::move(log), loc));
}

void emitNonPayableCheck(
	awst::SourceLocation const& loc,
	std::vector<std::shared_ptr<awst::Statement>>& out)
{
	// The normal method-body guard is keyed by the ARC4 selector. EVM entry
	// routes have a different selector, so enforce the same payment rule at the
	// adapter while the selected external function is known.
	auto groupIndex = awst::makeTxn(
		"GroupIndex", awst::WType::uint64Type(), loc);
	auto hasPrecedingTxn = awst::makeNumericCompare(
		groupIndex, awst::NumericComparison::Gt, u64(0, loc), loc);
	auto paymentIndex = awst::makeUInt64BinOp(
		awst::makeTxn("GroupIndex", awst::WType::uint64Type(), loc),
		awst::UInt64BinaryOperator::Sub, u64(1, loc), loc);
	auto amount = awst::makeGtxns(
		"Amount", std::move(paymentIndex), awst::WType::uint64Type(), loc);
	auto value = awst::makeConditional(
		std::move(hasPrecedingTxn), std::move(amount), u64(0, loc),
		awst::WType::uint64Type(), loc);
	out.push_back(awst::makeExpressionStatement(
		awst::makeAssert(
			awst::makeNumericCompare(std::move(value),
				awst::NumericComparison::Eq, u64(0, loc), loc),
			loc, "not payable"), loc));
}

struct EvmRoute
{
	FunctionType const* function = nullptr;
	awst::ContractMethod* method = nullptr;
	std::vector<uint8_t> selector;
};

/// Collect one EVM route per external Solidity function. In `quiet` mode a
/// method that cannot carry an EVM route is SKIPPED rather than an error: the
/// ARC-4 profile mounts these routes as a compatibility alias next to its
/// native router, so a non-EVM-able method merely has no alias — its ARC-4
/// route still serves it. The EVM profile has no other transport, so there the
/// same conditions stay hard errors.
std::vector<EvmRoute> collectEvmRoutes(
	ContractDefinition const& contractDefinition,
	awst::Contract& contract,
	OverloadedNamesSet const& overloadedNames,
	awst::SourceLocation const& loc,
	bool quiet)
{
	std::vector<EvmRoute> routes;
	for (auto const& [_, function]: contractDefinition.interfaceFunctionList(true))
	{
		if (!function)
			continue;
		auto name = methodNameFor(*function, overloadedNames);
		auto* method = findMethod(contract, name);
		if (!method)
		{
			if (!quiet)
				Logger::instance().error(
					"cannot build EVM entry route for Solidity function '"
						+ function->externalSignature() + "': generated method '"
						+ name + "' was not found", loc);
			continue;
		}
		if (!abi::canDecodeEvmAbi(function->parameterTypes())
			|| !abi::canEncodeEvmAbi(function->returnParameterTypes()))
		{
			if (!quiet)
				Logger::instance().error(
					"Solidity ABI entry route contains a type unsupported by the "
					"canonical recursive codec: " + function->externalSignature(), loc);
			continue;
		}
		// canEncodeEvmAbi answers the TYPE question, but emitting an external
		// function pointer additionally needs --evm-selectors (the default
		// profile's compact pointer stores the ARC-4 route, not the Solidity
		// selector, so the codec hard-errors). In quiet/alias mode such a method
		// simply keeps only its ARC-4 route.
		if (quiet)
		{
			std::function<bool(Type const*)> needsEvmSelectors =
				[&](Type const* type) -> bool {
					if (!type) return false;
					if (auto const* fn = dynamic_cast<FunctionType const*>(type))
						return fn->kind() == FunctionType::Kind::External;
					if (auto const* arr = dynamic_cast<ArrayType const*>(type))
						return needsEvmSelectors(arr->baseType());
					if (auto const* st = dynamic_cast<StructType const*>(type))
					{
						for (auto const& member: st->structDefinition().members())
							if (needsEvmSelectors(member->type()))
								return true;
						return false;
					}
					return false;
				};
			bool blocked = false;
			for (auto const* type: function->parameterTypes())
				blocked = blocked || needsEvmSelectors(type);
			for (auto const* type: function->returnParameterTypes())
				blocked = blocked || needsEvmSelectors(type);
			if (blocked)
				continue;
		}
		routes.push_back({function, method,
			SolcFacts::externalSelector(*function)});
	}
	return routes;
}

/// Emit one guarded dispatch arm for an EVM route into `sink`:
///   OnCompletion==NoOp && NumAppArgs==2 && Args[0]==keccak4(signature)
///   -> non-payable check, EVM-decode Args[1], call, EVM-encode + return log.
/// Shared verbatim by the EVM entry profile and the ARC-4 profile's
/// compatibility alias mount.
void emitEvmRouteArm(
	TypeMapper& typeMapper,
	EvmRoute const& route,
	std::vector<std::shared_ptr<awst::Statement>>& sink,
	awst::SourceLocation const& loc)
{
	auto const& paramTypes = route.function->parameterTypes();
	auto const& returnTypes = route.function->returnParameterTypes();
	auto selectorMatches = awst::makeBytesComparison(
		awst::makeAppArg(0, loc), awst::EqualityComparison::Eq,
		awst::makeBytesConstant(route.selector, loc,
			awst::BytesEncoding::Base16, awst::WType::bytesType()), loc);
	auto shapeMatches = awst::makeBoolBinOp(
		appArgCountIs(2, loc), awst::BinaryBooleanOperator::And,
		std::move(selectorMatches), loc);
	auto condition = awst::makeBoolBinOp(
		isNoOpCall(loc), awst::BinaryBooleanOperator::And,
		std::move(shapeMatches), loc);
	auto body = awst::makeBlock(loc);
	if (!route.function->isPayable())
		emitNonPayableCheck(loc, body->body);

	std::vector<std::shared_ptr<awst::Expression>> values;
	if (!paramTypes.empty())
	{
		awst::WType const* decodedType = nullptr;
		if (paramTypes.size() == 1)
			decodedType = typeMapper.map(paramTypes[0]);
		else
		{
			std::vector<awst::WType const*> tupleTypes;
			for (auto const* type: paramTypes)
				tupleTypes.push_back(typeMapper.map(type));
			decodedType = typeMapper.createType<awst::WTuple>(
				std::move(tupleTypes));
		}
		auto decoded = abi::decodeEvmAbi(
			typeMapper, awst::makeAppArg(1, loc), paramTypes,
			decodedType, loc, body->body);
		if (paramTypes.size() == 1)
			values.push_back(std::move(decoded));
		else
		{
			auto once = awst::makeEvalOnce(std::move(decoded), loc);
			auto const* tuple = dynamic_cast<awst::WTuple const*>(decodedType);
			for (size_t i = 0; i < paramTypes.size(); ++i)
				values.push_back(awst::makeTupleItem(
					once, static_cast<int>(i), tuple->types()[i], loc));
		}
	}

	auto call = awst::makeSubroutineCall(
		awst::InstanceMethodTarget{route.method->memberName},
		route.method->returnType, loc);
	for (size_t i = 0; i < values.size(); ++i)
	{
		auto value = std::move(values[i]);
		auto const* expected = i < route.method->args.size()
			? route.method->args[i].wtype : value->wtype;
		if (value->wtype != expected)
			value = codec::valueToArc4(
				typeMapper, paramTypes[i], std::move(value), expected, loc);
		awst::pushCallArg(call->args, std::move(value));
	}

	std::vector<std::shared_ptr<awst::Expression>> returnValues;
	if (returnTypes.empty())
	{
		body->body.push_back(awst::makeExpressionStatement(call, loc));
	}
	else if (returnTypes.size() == 1)
		returnValues.push_back(call);
	else
	{
		auto once = awst::makeEvalOnce(call, loc);
		auto const* tuple = dynamic_cast<awst::WTuple const*>(
			route.method->returnType);
		for (size_t i = 0; i < returnTypes.size(); ++i)
			returnValues.push_back(awst::makeTupleItem(
				once, static_cast<int>(i), tuple->types()[i], loc));
	}
	auto encoded = abi::encodeEvmAbi(
		typeMapper, returnTypes, std::move(returnValues), loc, body->body);
	emitReturnLog(std::move(encoded), loc, body->body);
	body->body.push_back(awst::makeReturnStatement(awst::makeTrue(loc), loc));
	sink.push_back(awst::makeIfElse(
		std::move(condition), std::move(body), nullptr, loc));
}
}

void ContractBuilder::emitEvmEntryDispatch(
	ContractDefinition const& contractDefinition,
	awst::Contract& contract)
{
	auto& approval = contract.approvalProgram;
	if (!approval.body)
		return;
	auto const& loc = approval.sourceLocation;

	auto routes = collectEvmRoutes(
		contractDefinition, contract, m_overloadedNames, loc, /*quiet=*/false);
	// The methods remain ordinary callable subroutines, but are no longer
	// advertised to or dispatched by puya's ARC4 router.
	for (auto const& route: routes)
		route.method->arc4MethodConfig.reset();

	// Solidity fallback/receive are owned by this adapter as well. Their full
	// forwarding behavior is added below; suppress accidental ARC4 exposure.
	for (auto& method: contract.methods)
		if (method.memberName == "__fallback" || method.memberName == "__receive")
			method.arc4MethodConfig.reset();
	auto const* fallbackDefinition = contractDefinition.fallbackFunction();
	if (fallbackDefinition && !fallbackDefinition->isImplemented())
		fallbackDefinition = nullptr;
	auto const* receiveDefinition = contractDefinition.receiveFunction();
	if (receiveDefinition && !receiveDefinition->isImplemented())
		receiveDefinition = nullptr;
	auto* fallbackMethod = fallbackDefinition
		? findMethod(contract, "__fallback") : nullptr;
	auto* receiveMethod = receiveDefinition
		? findMethod(contract, "__receive") : nullptr;

	// Compiler-private lifecycle methods (notably __postInit) retain ARC4
	// configs. Give that residual router first refusal without reopening any
	// public Solidity route under ARC4 selectors.
	bool hasResidualArc4Route = false;
	for (auto const& method: contract.methods)
		if (method.arc4MethodConfig)
		{
			hasResidualArc4Route = true;
			break;
		}
	if (hasResidualArc4Route)
	{
		std::string didName = "__evm_entry_arc4_internal";
		auto did = [&]() {
			return awst::makeVarExpression(didName, awst::WType::boolType(), loc);
		};
		approval.body->body.push_back(awst::makeAssignmentStatement(
			did(), awst::makeARC4Router(awst::WType::boolType(), loc), loc));
		auto accepted = awst::makeBlock(loc);
		accepted->body.push_back(awst::makeReturnStatement(awst::makeTrue(loc), loc));
		approval.body->body.push_back(awst::makeIfElse(
			did(), std::move(accepted), nullptr, loc));
	}

	// Empty Solidity calldata selects receive(), or fallback() when receive is
	// absent. Emit the carrier log even for a void handler so low-level callers
	// have one deterministic return record to capture.
	if (receiveMethod || fallbackMethod)
	{
		auto* emptyTarget = receiveMethod ? receiveMethod : fallbackMethod;
		auto const* emptyDefinition = receiveMethod
			? receiveDefinition : fallbackDefinition;
		auto condition = awst::makeBoolBinOp(
			isNoOpCall(loc), awst::BinaryBooleanOperator::And,
			appArgCountIs(0, loc), loc);
		auto body = awst::makeBlock(loc);
		if (emptyDefinition && !emptyDefinition->isPayable())
			emitNonPayableCheck(loc, body->body);
		auto call = awst::makeSubroutineCall(
			awst::InstanceMethodTarget{emptyTarget->memberName},
			emptyTarget->returnType, loc);
		if (emptyDefinition && !emptyDefinition->parameters().empty())
			awst::pushCallArg(call->args, awst::makeBytesConstant({}, loc));
		if (emptyTarget->returnType == awst::WType::voidType())
		{
			body->body.push_back(awst::makeExpressionStatement(call, loc));
			emitReturnLog(awst::makeBytesConstant({}, loc), loc, body->body);
		}
		else
			emitReturnLog(call, loc, body->body);
		body->body.push_back(awst::makeReturnStatement(awst::makeTrue(loc), loc));
		approval.body->body.push_back(awst::makeIfElse(
			std::move(condition), std::move(body), nullptr, loc));
	}

	for (auto const& route: routes)
		emitEvmRouteArm(m_typeMapper, route, approval.body->body, loc);

	// Unmatched non-empty calldata selects fallback(). Reconstruct exactly the
	// Solidity byte stream from the AVM carrier split: selector ++ ABI body.
	if (fallbackMethod)
	{
		auto hasSelector = awst::makeNumericCompare(
			awst::makeTxn("NumAppArgs", awst::WType::uint64Type(), loc),
			awst::NumericComparison::Gt, u64(0, loc), loc);
		auto hasBody = awst::makeNumericCompare(
			awst::makeTxn("NumAppArgs", awst::WType::uint64Type(), loc),
			awst::NumericComparison::Gt, u64(1, loc), loc);
		auto bodyBytes = awst::makeConditional(
			std::move(hasBody), awst::makeAppArg(1, loc),
			awst::makeBytesConstant({}, loc), awst::WType::bytesType(), loc);
		auto calldata = awst::makeConditional(
			std::move(hasSelector),
			awst::makeConcat(awst::makeAppArg(0, loc), std::move(bodyBytes), loc),
			awst::makeBytesConstant({}, loc), awst::WType::bytesType(), loc);
		auto carrierShape = awst::makeBoolBinOp(
			awst::makeNumericCompare(
				awst::makeTxn("NumAppArgs", awst::WType::uint64Type(), loc),
				awst::NumericComparison::Gt, u64(0, loc), loc),
			awst::BinaryBooleanOperator::And,
			awst::makeNumericCompare(
				awst::makeTxn("NumAppArgs", awst::WType::uint64Type(), loc),
				awst::NumericComparison::Lte, u64(2, loc), loc), loc);
		auto condition = awst::makeBoolBinOp(
			isNoOpCall(loc), awst::BinaryBooleanOperator::And,
			std::move(carrierShape), loc);
		auto fallbackBody = awst::makeBlock(loc);
		if (fallbackDefinition && !fallbackDefinition->isPayable())
			emitNonPayableCheck(loc, fallbackBody->body);
		auto call = awst::makeSubroutineCall(
			awst::InstanceMethodTarget{fallbackMethod->memberName},
			fallbackMethod->returnType, loc);
		if (fallbackDefinition && !fallbackDefinition->parameters().empty())
			awst::pushCallArg(call->args, std::move(calldata));
		if (fallbackMethod->returnType == awst::WType::voidType())
		{
			fallbackBody->body.push_back(
				awst::makeExpressionStatement(call, loc));
			emitReturnLog(awst::makeBytesConstant({}, loc), loc,
				fallbackBody->body);
		}
		else
			emitReturnLog(call, loc, fallbackBody->body);
		fallbackBody->body.push_back(
			awst::makeReturnStatement(awst::makeTrue(loc), loc));
		approval.body->body.push_back(awst::makeIfElse(
			std::move(condition), std::move(fallbackBody), nullptr, loc));
	}

	// No matching Solidity selector and no compiler-private ARC4 route.
	approval.body->body.push_back(
		awst::makeReturnStatement(awst::makeFalse(loc), loc));
}


void ContractBuilder::emitEvmCompatRoutes(
	solidity::frontend::ContractDefinition const& contractDefinition,
	awst::Contract& contract)
{
	auto& approval = contract.approvalProgram;
	if (!approval.body)
		return;
	auto const& loc = approval.sourceLocation;

	// ARC-4 profile: the abi.* builtins emit canonical EVM calldata in every
	// profile, so `target.call(abi.encodeWithSignature(...))` reaches an
	// ARC-4-routed callee carrying a keccak selector — which the native router
	// errs on. Mount the same EVM route arms the --contract-abi evm profile
	// uses AS AN ALIAS, ahead of the untouched ARC-4 router: every arm is
	// guarded on NumAppArgs==2 (the [selector, body] carrier the low-level
	// call lowering emits), so native ARC-4 traffic is dispatched exactly as
	// before. arc4MethodConfigs are NOT reset — ARC-4 stays the primary
	// transport; methods whose types cannot round-trip the EVM codec simply
	// have no alias (quiet mode) and keep their ARC-4 route.
	auto routes = collectEvmRoutes(
		contractDefinition, contract, m_overloadedNames, loc, /*quiet=*/true);
	for (auto const& route: routes)
		emitEvmRouteArm(m_typeMapper, route, approval.body->body, loc);
}

} // namespace puyasol::builder
