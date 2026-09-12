#include "builder/itxn/ApplicationCall.h"
#include "builder/itxn/ApplicationTarget.h"
#include "builder/BuildArtifacts.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/codec/EvmValueCodec.h"
#include "builder/abi/EvmAbiEncode.h"
#include "awst/NameGen.h"
#include "awst/TupleValue.h"

namespace puyasol::builder
{

ApplicationCall::Expr ApplicationCall::submit(TypeMapper& types, Expr receiver,
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
	return capture(types, loc, out);
}

ApplicationCall::Expr ApplicationCall::submitRaw(TypeMapper& types, Expr receiver,
	Expr bytes, Expr payment, awst::SourceLocation const& loc, Statements& out)
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
	if (auto create = std::dynamic_pointer_cast<awst::CreateInnerTransaction>(payment))
		for (auto& [name, field]: create->fields) field = pin(std::move(field));
	auto result = awst::makeVarExpression("__raw_result_" + std::to_string(
		awst::NameGen::next("ApplicationCall.rawResult")), awst::WType::bytesType(), loc);
	auto empty = awst::makeBlock(loc), nonEmpty = awst::makeBlock(loc);
	auto emptyResult = submit(types, receiver, nullptr, payment, loc, empty->body);
	empty->body.push_back(awst::makeAssignmentStatement(result, std::move(emptyResult), loc));
	auto fullResult = submit(types, receiver, splitPayload(types, input, loc), payment, loc, nonEmpty->body);
	nonEmpty->body.push_back(awst::makeAssignmentStatement(result, std::move(fullResult), loc));
	out.push_back(awst::makeIfElse(awst::makeNumericCompare(awst::makeLen(input, loc),
		awst::NumericComparison::Eq, awst::makeZero(loc), loc), std::move(empty), std::move(nonEmpty), loc));
	return result;
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

} // namespace puyasol::builder
