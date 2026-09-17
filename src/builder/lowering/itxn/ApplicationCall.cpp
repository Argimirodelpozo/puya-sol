#include "builder/lowering/itxn/ApplicationCall.h"
#include "builder/lowering/itxn/NativePayment.h"
#include "builder/target/ApplicationTarget.h"
#include "builder/context/BuildArtifacts.h"
#include "builder/types/TypeMapper.h"
#include "builder/types/TypeCoercion.h"
#include "builder/codec/EvmValueCodec.h"
#include "builder/codec/EvmAbiEncode.h"
#include "awst/NameGen.h"
#include "awst/TupleValue.h"

namespace puyasol::builder
{

ApplicationCall::Expr ApplicationCall::encodeArguments(TypeMapper& types, Expr selector,
	std::vector<solidity::frontend::Type const*> const& parameters,
	std::vector<Expr> values, awst::SourceLocation const& loc, Statements& out)
{
	assert(parameters.size() == values.size());
	auto arguments = awst::makeTupleExpression(nullptr, loc);
	arguments->items.push_back(std::move(selector));
	if (types.profile().contractAbi == ContractAbi::Evm)
		arguments->items.push_back(abi::encodeEvmAbi(types, parameters, std::move(values), loc, out));
	else
		for (size_t i = 0; i < values.size(); ++i)
		{
			// Public parameters use call-boundary carriers, not aggregate widths.
			CallParameterPlan parameter;
			parameter.type = values[i]->wtype;
			parameter.setAbiWireType(types, parameters[i]);
			arguments->items.push_back(awst::makeAsBytes(codec::valueToArc4(types, parameters[i],
				std::move(values[i]), types.mapToARC4Type(parameter.wireType), loc), loc));
		}
	arguments->wtype = types.createType<awst::WTuple>(
		std::vector<awst::WType const*>(arguments->items.size(), awst::WType::bytesType()));
	return arguments;
}

ApplicationCall::Expr ApplicationCall::submit(TypeMapper& types, Expr receiver,
	Expr arguments, Expr payment, awst::SourceLocation const& loc, Statements& out)
{
	submitOnly(types, std::move(receiver), std::move(arguments), std::move(payment), loc, out);
	return capture(types, loc, out);
}

void ApplicationCall::submitOnly(TypeMapper& types, Expr receiver,
	Expr arguments, Expr payment, awst::SourceLocation const& loc, Statements& out)
{
	static awst::WInnerTransactionFields fields(6);
	static awst::WInnerTransaction transaction(6);
	auto create = awst::makeCreateInnerTransaction(&fields, loc);
	create->fields["TypeEnum"] = awst::makeIntegerConstant("6", loc);
	create->fields["Fee"] = awst::makeZero(loc);
	create->fields["OnCompletion"] = awst::makeZero(loc);
	create->fields["ApplicationID"] = ApplicationTarget::requireApplication(
		ApplicationTarget::resolve(types.profile(), std::move(receiver), loc), loc);
	if (arguments) create->fields["ApplicationArgs"] = std::move(arguments);
	auto submit = awst::makeSubmitInnerTransaction(&transaction, loc);
	if (payment) submit->itxns.push_back(std::move(payment));
	submit->itxns.push_back(std::move(create));
	out.push_back(awst::makeExpressionStatement(std::move(submit), loc));
}

ApplicationCall::Expr ApplicationCall::submitRaw(TypeMapper& types, Expr receiver,
	Expr bytes, Expr amount, awst::SourceLocation const& loc, Statements& out)
{
	auto input = awst::makeVarExpression("__raw_input_" + std::to_string(
		awst::NameGen::next("ApplicationCall.rawInput")), awst::WType::bytesType(), loc);
	out.push_back(awst::makeAssignmentStatement(input, std::move(bytes), loc));
	auto pin = [&](Expr value) {
		auto local = awst::makeVarExpression("__raw_operand_" + std::to_string(
			awst::NameGen::next("ApplicationCall.rawOperand")), value->wtype, loc);
		out.push_back(awst::makeAssignmentStatement(local, std::move(value), loc));
		return local;
	};
	receiver = pin(std::move(receiver));
	if (amount) amount = pin(std::move(amount));
	auto empty = awst::makeBlock(loc), nonEmpty = awst::makeBlock(loc);
	empty->body.push_back(buildNativeTransfer(types, empty->body, receiver, amount, loc));
	auto payment = amount ? buildNativePayment(types.profile(), nonEmpty->body, receiver, amount, loc) : nullptr;
	submit(types, receiver, splitPayload(types, input, loc), std::move(payment), loc, nonEmpty->body);
	out.push_back(awst::makeIfElse(awst::makeNumericCompare(awst::makeLen(input, loc),
		awst::NumericComparison::Eq, awst::makeZero(loc), loc), std::move(empty), std::move(nonEmpty), loc));
	return pin(returnData(types, loc));
}

ApplicationCall::Expr ApplicationCall::capture(TypeMapper& types,
	awst::SourceLocation const& loc, Statements& out)
{
	auto log = awst::makeEvalOnce(awst::makeConditional(
		awst::makeNumericCompare(awst::makeItxn("NumLogs", awst::WType::uint64Type(), loc),
			awst::NumericComparison::Gt, awst::makeZero(loc), loc),
		awst::makeItxn("LastLog", awst::WType::bytesType(), loc),
		awst::makeBytesConstant({}, loc), awst::WType::bytesType(), loc), loc);
	auto prefixed = awst::makeConditional(
		awst::makeNumericCompare(awst::makeLen(log, loc), awst::NumericComparison::Gte,
			awst::makeIntegerConstant("4", loc), loc),
		awst::makeBytesComparison(awst::makeExtract(log, 0, 4, loc), awst::EqualityComparison::Eq,
			awst::makeBytesConstant({0x15, 0x1f, 0x7c, 0x75}, loc), loc),
		awst::makeFalse(loc), awst::WType::boolType(), loc);
	return setReturnData(types, awst::makeConditional(prefixed,
		awst::makeExtract(log, 4, 0, loc), awst::makeBytesConstant({}, loc),
		awst::WType::bytesType(), loc), loc, out);
}

ApplicationCall::Expr ApplicationCall::setReturnData(TypeMapper& types, Expr bytes,
	awst::SourceLocation const& loc, Statements& out)
{
	types.artifacts().usesReturnData = true;
	auto value = awst::makeVarExpression("__return_data_" + std::to_string(
		awst::NameGen::next("ApplicationCall.returnData")), awst::WType::bytesType(), loc);
	out.push_back(awst::makeAssignmentStatement(value, std::move(bytes), loc));
	out.push_back(awst::makeExpressionStatement(
		awst::makeStoreSlot(ScratchLayout::returnDataSlot, value, loc), loc));
	return value;
}

ApplicationCall::Expr ApplicationCall::setTypedReturnData(TypeMapper& types, Expr value,
	std::vector<solidity::frontend::Type const*> const& returns, bool evmWire,
	awst::SourceLocation const& loc, Statements& out)
{
	if (returns.empty())
	{
		out.push_back(awst::makeExpressionStatement(std::move(value), loc));
		return setReturnData(types, awst::makeBytesConstant({}, loc), loc, out);
	}
	value = awst::makeEvalOnce(std::move(value), loc);
	auto values = returns.size() == 1 ? std::vector<Expr>{value} : awst::tupleItems(value, loc);
	if (evmWire)
		return setReturnData(types, abi::encodeEvmAbi(types, returns, std::move(values), loc, out), loc, out);
	std::vector<awst::WType const*> wireTypes;
	for (size_t i = 0; i < returns.size(); ++i)
	{
		auto plan = planReturnElement(types, returns[i], abiReturnNativeType(types, returns[i]));
		values[i] = TypeCoercion::encodeReturnElement(
			codec::valueFromArc4(types, returns[i], std::move(values[i]), loc), plan, loc);
		auto wire = types.mapToARC4Type(plan.wireType);
		values[i] = codec::valueToArc4(types, returns[i], std::move(values[i]), wire, loc);
		wireTypes.push_back(wire);
	}
	Expr encoded = values.front();
	if (values.size() > 1)
	{
		auto tuple = awst::makeTupleExpression(types.createType<awst::WTuple>(wireTypes), loc);
		tuple->items = std::move(values);
		encoded = awst::makeARC4Encode(std::move(tuple), types.createType<awst::ARC4Tuple>(wireTypes), loc);
	}
	return setReturnData(types, awst::makeAsBytes(std::move(encoded), loc), loc, out);
}

ApplicationCall::Expr ApplicationCall::returnData(TypeMapper& types, awst::SourceLocation const& loc)
{
	types.artifacts().usesReturnData = true;
	return awst::makeLoadSlot(ScratchLayout::returnDataSlot, loc);
}

ApplicationCall::Expr ApplicationCall::splitPayload(TypeMapper& types, Expr bytes,
	awst::SourceLocation const& loc)
{
	bytes = awst::makeEvalOnce(std::move(bytes), loc);
	auto hasSelector = awst::makeNumericCompare(awst::makeLen(bytes, loc),
		awst::NumericComparison::Gte, awst::makeIntegerConstant("4", loc), loc);
	auto selector = awst::makeConditional(hasSelector, awst::makeExtract(bytes, 0, 4, loc),
		bytes, awst::WType::bytesType(), loc);
	auto body = awst::makeConditional(hasSelector, awst::makeExtract(bytes, 4, 0, loc),
		awst::makeBytesConstant({}, loc), awst::WType::bytesType(), loc);
	auto tuple = awst::makeTupleExpression(types.createType<awst::WTuple>(
		std::vector<awst::WType const*>{awst::WType::bytesType(), awst::WType::bytesType()}), loc);
	tuple->items = {std::move(selector), std::move(body)};
	return tuple;
}

ApplicationCall::Expr ApplicationCall::staticContext(TypeMapper& types, awst::SourceLocation const& loc)
{
	types.artifacts().usesStaticContext = true;
	return awst::makeIntrinsicCall("load", awst::WType::uint64Type(), loc, {ScratchLayout::staticContextSlot});
}

ApplicationCall::Expr ApplicationCall::withStaticContext(TypeMapper& types, Expr value,
	bool staticCall, awst::SourceLocation const& loc, Statements& out)
{
	if (!staticCall) return value;
	auto saved = awst::makeVarExpression("__static_context_" + std::to_string(
		awst::NameGen::next("ApplicationCall.staticContext")), awst::WType::uint64Type(), loc);
	out.push_back(awst::makeAssignmentStatement(saved, staticContext(types, loc), loc));
	out.push_back(awst::makeExpressionStatement(awst::makeStoreSlot(
		ScratchLayout::staticContextSlot, awst::makeIntegerConstant("1", loc), loc), loc));
	if (value->wtype == awst::WType::voidType())
	{
		out.push_back(awst::makeExpressionStatement(std::move(value), loc));
		value = awst::makeVoidConstant(loc);
	}
	else
	{
		auto result = awst::makeVarExpression(saved->name + "_result", value->wtype, loc);
		out.push_back(awst::makeAssignmentStatement(result, std::move(value), loc));
		value = std::move(result);
	}
	out.push_back(awst::makeExpressionStatement(awst::makeStoreSlot(
		ScratchLayout::staticContextSlot, std::move(saved), loc), loc));
	return value;
}

} // namespace puyasol::builder
