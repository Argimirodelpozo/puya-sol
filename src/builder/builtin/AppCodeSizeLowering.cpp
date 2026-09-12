#include "builder/builtin/AppCodeSizeLowering.h"

#include "awst/NameGen.h"
#include "builder/sol-types/TypeMapper.h"

namespace puyasol::builder
{

std::shared_ptr<awst::Expression> AppCodeSizeLowering::lower(
	TypeMapper& _typeMapper,
	std::shared_ptr<awst::Expression> _application,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _effects,
	bool _inConstructor)
{
	using namespace awst;
	std::string const prefix = "__app_code_" + std::to_string(
		NameGen::next("AppCodeSizeLowering.s_idCounter") + 1);
	auto id = [&] { return makeVarExpression(prefix + "_id", WType::uint64Type(), _loc); };
	auto size = [&] { return makeVarExpression(prefix + "_size", WType::uint64Type(), _loc); };
	_effects.push_back(makeAssignmentStatement(id(), makeAsUInt64(std::move(_application), _loc), _loc));
	_effects.push_back(makeAssignmentStatement(size(), makeZero(_loc), _loc));

	// AVM reference zero aliases self, not an EOA. Construction also has no
	// deployed self code. Apply both guards here for Solidity and Yul callers.
	auto readable = makeNumericCompare(id(), NumericComparison::Ne, makeZero(_loc), _loc);
	if (_inConstructor)
		readable = makeNumericCompare(
			makeConditional(readable, id(), makeGlobal("CurrentApplicationID", WType::uint64Type(), _loc),
				WType::uint64Type(), _loc), NumericComparison::Ne,
			makeGlobal("CurrentApplicationID", WType::uint64Type(), _loc), _loc);

	auto* tupleType = _typeMapper.createType<WTuple>(
		std::vector<WType const*>{WType::uint64Type(), WType::boolType()});
	auto tuple = [&] { return makeVarExpression(prefix + "_query", tupleType, _loc); };
	auto body = makeBlock(_loc);
	body->body.push_back(makeAssignmentStatement(tuple(),
		makeAppParamsGet("AppExtraProgramPages", id(), tupleType, _loc), _loc));
	auto capacity = makeUInt64BinOp(makeUInt64BinOp(
		makeTupleItem(tuple(), 0, WType::uint64Type(), _loc),
		UInt64BinaryOperator::Add, makeIntegerConstant(1, _loc), _loc),
		UInt64BinaryOperator::Mult, makeIntegerConstant(2048, _loc), _loc);
	// Consume the tuple immediately in this branch. Only a scalar escapes into
	// the surrounding expression, including Yul memory/returndata expressions.
	body->body.push_back(makeAssignmentStatement(size(), makeConditional(
		makeTupleItem(tuple(), 1, WType::boolType(), _loc), std::move(capacity),
		makeZero(_loc), WType::uint64Type(), _loc), _loc));
	_effects.push_back(makeIfElse(std::move(readable), std::move(body), nullptr, _loc));
	return size();
}

} // namespace puyasol::builder
