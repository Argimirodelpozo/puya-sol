#include "builder/lowering/itxn/ApplicationCall.h"
#include "builder/lowering/itxn/NativePayment.h"
#include "builder/target/ApplicationTarget.h"
#include "builder/context/BuildArtifacts.h"
#include "builder/context/ContractContext.h"
#include "builder/context/TranslationContext.h"
#include "builder/context/ProgramAnalysis.h"
#include "builder/types/TypeMapper.h"
#include "builder/types/TypeCoercion.h"
#include "builder/codec/EvmValueCodec.h"
#include "builder/codec/EvmAbiEncode.h"
#include "builder/codec/EvmAbiDecode.h"
#include "builder/codec/Arc4Defaults.h"
#include "awst/NameGen.h"
#include "awst/TupleValue.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder
{

namespace
{
using Expr = ApplicationCall::Expr;
using Statements = ApplicationCall::Statements;

bool canReturnRaw(TypeMapper& types)
{
	auto const& analysis = types.analysis();
	auto& artifacts = types.artifacts();
	if (artifacts.currentFreestandingFunctionId >= 0)
		return analysis.callablesWithRawReturn.contains(artifacts.currentFreestandingFunctionId);
	for (auto id: analysis.callablesWithRawReturn)
		if (analysis.isCallableReachable(artifacts.contract().sourceId, id)) return true;
	return false;
}

Expr captureValue(Expr value, awst::SourceLocation const& loc, Statements& out)
{
	if (value->wtype == awst::WType::voidType())
	{
		out.push_back(awst::makeExpressionStatement(std::move(value), loc));
		return awst::makeVoidConstant(loc);
	}
	auto saved = awst::makeVarExpression("__call_value_" + std::to_string(
		awst::NameGen::next("ApplicationCall.value")), value->wtype, loc);
	out.push_back(awst::makeAssignmentStatement(saved, std::move(value), loc));
	return saved;
}

Expr frameFlag(TypeMapper& types, int slot, awst::SourceLocation const& loc)
{
	types.artifacts().noteScratchUse(slot);
	return awst::makeIntrinsicCall("load", awst::WType::uint64Type(), loc, {slot});
}

std::shared_ptr<awst::ReturnStatement> emptyFrameReturn(awst::WType const* type, awst::SourceLocation const& loc)
{
	return awst::makeReturnStatement(!type || type == awst::WType::voidType()
		? nullptr : TypeCoercion::makeDefaultValue(type, loc), loc);
}
} // namespace

ApplicationCall::Expr ApplicationCall::selfCallContext(TypeMapper& types, awst::SourceLocation const& loc)
{
	return awst::makeNumericCompare(frameFlag(types, ScratchLayout::selfCallFrameSlot, loc),
		awst::NumericComparison::Ne, awst::makeZero(loc), loc);
}

void ApplicationCall::returnRaw(TypeMapper& types, Expr bytes, awst::WType const* frameType,
	awst::SourceLocation const& loc, Statements& out)
{
	types.artifacts().noteScratchUse(ScratchLayout::rawReturnFlagSlot);
	auto self = awst::makeBlock(loc), entry = awst::makeBlock(loc);
	setReturnData(types, bytes, loc, self->body);
	self->body.push_back(awst::makeExpressionStatement(awst::makeStoreSlot(
		ScratchLayout::rawReturnFlagSlot, awst::makeOne(loc), loc), loc));
	self->body.push_back(emptyFrameReturn(frameType, loc));
	auto log = awst::makeIntrinsicCall("log", awst::WType::voidType(), loc);
	log->stackArgs.push_back(awst::makeConcat(awst::makeBytesConstant({0x15, 0x1f, 0x7c, 0x75}, loc), bytes, loc));
	entry->body.push_back(awst::makeExpressionStatement(std::move(log), loc));
	auto halt = awst::makeIntrinsicCall("return", awst::WType::voidType(), loc);
	halt->stackArgs.push_back(awst::makeTrue(loc));
	entry->body.push_back(awst::makeExpressionStatement(std::move(halt), loc));
	out.push_back(awst::makeIfElse(selfCallContext(types, loc), std::move(self), std::move(entry), loc));
}

ApplicationCall::Expr ApplicationCall::propagateRawReturn(TypeMapper& types, Expr call,
	awst::WType const* frameType, awst::SourceLocation const& loc, Statements& out)
{
	if (!canReturnRaw(types)) return call;
	auto value = captureValue(std::move(call), loc, out);
	auto halted = awst::makeBlock(loc);
	halted->body.push_back(emptyFrameReturn(frameType, loc));
	out.push_back(awst::makeIfElse(awst::makeNumericCompare(
		frameFlag(types, ScratchLayout::rawReturnFlagSlot, loc), awst::NumericComparison::Ne,
		awst::makeZero(loc), loc), std::move(halted), nullptr, loc));
	return value;
}

ApplicationCall::Expr ApplicationCall::propagateRawReturn(eb::ContractContext& context, Expr call,
	awst::SourceLocation const& loc)
{
	auto const* function = context.scope().function;
	if (!function) return call;
	auto const id = function->sourceFunction ? function->sourceFunction->id() : function->callableId;
	if (!context.typeMapper.analysis().callablesWithRawReturn.contains(id)) return call;
	auto const* frameType = function->returnType;
	if (function->encodeReturnsAtBuildTime)
		frameType = context.typeMapper.functionReturnPlan(
			*context.typeMapper.analysis().functionDeclarations.at(function->callableId)).wireType;
	return propagateRawReturn(context.typeMapper, std::move(call), frameType, loc, context.preEffects());
}

ApplicationCall::Expr ApplicationCall::decodeRawReturn(TypeMapper& types, Expr value,
	std::vector<solidity::frontend::Type const*> const& returns,
	awst::SourceLocation const& loc, Statements& out)
{
	if (!canReturnRaw(types) || returns.empty()) return value;
	value = captureValue(std::move(value), loc, out);
	auto decoded = awst::makeBlock(loc);
	auto result = abi::decodeEvmAbi(types, returnData(types, loc), returns, value->wtype, loc, decoded->body);
	auto adapt = [&](Expr item, size_t i, awst::WType const* target) {
		return isArc4EncodedType(target)
			? codec::valueToArc4(types, returns[i], std::move(item), target, loc)
			: decodeCallResult(std::move(item), target, loc);
	};
	if (returns.size() == 1)
		result = adapt(std::move(result), 0, value->wtype);
	else
	{
		auto items = awst::tupleItems(std::move(result), loc);
		auto tuple = awst::makeTupleExpression(value->wtype, loc);
		auto const& targets = static_cast<awst::WTuple const*>(value->wtype)->types();
		for (size_t i = 0; i < items.size(); ++i)
			tuple->items.push_back(adapt(std::move(items[i]), i, targets[i]));
		result = std::move(tuple);
	}
	decoded->body.push_back(awst::makeAssignmentStatement(value, std::move(result), loc));
	if (types.profile().contractAbi != ContractAbi::Evm)
		decoded->body.push_back(awst::makeExpressionStatement(awst::makeStoreSlot(
			ScratchLayout::rawReturnFlagSlot, awst::makeZero(loc), loc), loc));
	out.push_back(awst::makeIfElse(awst::makeNumericCompare(
		frameFlag(types, ScratchLayout::rawReturnFlagSlot, loc), awst::NumericComparison::Ne,
		awst::makeZero(loc), loc), std::move(decoded), nullptr, loc));
	return value;
}

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
	types.artifacts().noteScratchUse(ScratchLayout::returnDataSlot);
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
	if (!canReturnRaw(types))
		return setTypedReturnDataUnchecked(types, std::move(value), returns, evmWire, loc, out);
	value = captureValue(std::move(value), loc, out);
	auto raw = awst::makeBlock(loc), normal = awst::makeBlock(loc);
	auto result = awst::makeVarExpression("__call_data_" + std::to_string(
		awst::NameGen::next("ApplicationCall.data")), awst::WType::bytesType(), loc);
	raw->body.push_back(awst::makeAssignmentStatement(result, returnData(types, loc), loc));
	auto encoded = setTypedReturnDataUnchecked(types, std::move(value), returns, evmWire, loc, normal->body);
	normal->body.push_back(awst::makeAssignmentStatement(result, std::move(encoded), loc));
	out.push_back(awst::makeIfElse(awst::makeNumericCompare(
		frameFlag(types, ScratchLayout::rawReturnFlagSlot, loc), awst::NumericComparison::Ne,
		awst::makeZero(loc), loc), std::move(raw), std::move(normal), loc));
	out.push_back(awst::makeExpressionStatement(awst::makeStoreSlot(
		ScratchLayout::rawReturnFlagSlot, awst::makeZero(loc), loc), loc));
	return result;
}

ApplicationCall::Expr ApplicationCall::finishSelfCall(TypeMapper& types, Expr bytes,
	awst::SourceLocation const& loc, Statements& out)
{
	if (!canReturnRaw(types))
		return setReturnData(types, std::move(bytes), loc, out);
	bytes = captureValue(std::move(bytes), loc, out);
	auto result = setReturnData(types, awst::makeConditional(awst::makeNumericCompare(
		frameFlag(types, ScratchLayout::rawReturnFlagSlot, loc), awst::NumericComparison::Ne,
		awst::makeZero(loc), loc), returnData(types, loc), std::move(bytes),
		awst::WType::bytesType(), loc), loc, out);
	out.push_back(awst::makeExpressionStatement(awst::makeStoreSlot(
		ScratchLayout::rawReturnFlagSlot, awst::makeZero(loc), loc), loc));
	return result;
}

ApplicationCall::Expr ApplicationCall::setTypedReturnDataUnchecked(TypeMapper& types, Expr value,
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
	types.artifacts().noteScratchUse(ScratchLayout::returnDataSlot);
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
	types.artifacts().noteScratchUse(ScratchLayout::staticContextSlot);
	return awst::makeIntrinsicCall("load", awst::WType::uint64Type(), loc, {ScratchLayout::staticContextSlot});
}

ApplicationCall::Expr ApplicationCall::withStaticContext(TypeMapper& types, Expr value,
	bool staticCall, awst::SourceLocation const& loc, Statements& out)
{
	bool const rawFrames = canReturnRaw(types);
	if (!staticCall && !rawFrames) return value;
	auto saved = awst::makeVarExpression("__static_context_" + std::to_string(
		awst::NameGen::next("ApplicationCall.staticContext")), awst::WType::uint64Type(), loc);
	if (staticCall)
	{
		out.push_back(awst::makeAssignmentStatement(saved, staticContext(types, loc), loc));
		out.push_back(awst::makeExpressionStatement(awst::makeStoreSlot(
			ScratchLayout::staticContextSlot, awst::makeOne(loc), loc), loc));
	}
	auto frame = awst::makeVarExpression(saved->name + "_frame", awst::WType::uint64Type(), loc);
	if (rawFrames)
	{
		out.push_back(awst::makeAssignmentStatement(frame, frameFlag(types, ScratchLayout::selfCallFrameSlot, loc), loc));
		out.push_back(awst::makeExpressionStatement(awst::makeStoreSlot(
			ScratchLayout::selfCallFrameSlot, awst::makeOne(loc), loc), loc));
	}
	value = captureValue(std::move(value), loc, out);
	if (rawFrames)
		out.push_back(awst::makeExpressionStatement(awst::makeStoreSlot(ScratchLayout::selfCallFrameSlot, frame, loc), loc));
	if (staticCall)
		out.push_back(awst::makeExpressionStatement(awst::makeStoreSlot(ScratchLayout::staticContextSlot, saved, loc), loc));
	return value;
}

} // namespace puyasol::builder
