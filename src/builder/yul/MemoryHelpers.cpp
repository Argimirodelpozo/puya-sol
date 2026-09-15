/// @file MemoryHelpers.cpp
/// Word conversion and exact byte-range scratch-memory access.

#include "builder/yul/AssemblyBuilder.h"
#include "builder/AwstShorthand.h"
#include "awst/NameGen.h"

#include <sstream>
#include <libyul/AST.h>
#include <libyul/Dialect.h>

namespace puyasol::builder
{

std::shared_ptr<awst::Expression> AssemblyBuilder::readMemSlot(
	uint64_t _offset,
	awst::SourceLocation const& _loc
)
{
	// Slot-routed (M7): offsets ≥ SLOT_SIZE read the right scratch slot
	// instead of running off the end of slot 0.
	return awst::makeAsBiguint(
		readMemWordDirect(m_typeMapper, awst::makeIntegerConstant(_offset, _loc), _loc), _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::padTo32Bytes(
	std::shared_ptr<awst::Expression> _expr,
	awst::SourceLocation const& _loc
)
{
	if (_expr->wtype == awst::WType::uint64Type())
		_expr = awst::makeItob(std::move(_expr), _loc);
	auto cast = awst::makeAsBytes(std::move(_expr), _loc);
	auto concatPad = awst::makeLeftPad(std::move(cast), 32, _loc);
	return awst::makeExtractLastN(std::move(concatPad), 32, _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::readMemRangeDyn(
	std::shared_ptr<awst::Expression> _offset,
	std::shared_ptr<awst::Expression> _length,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out)
{
	auto name = "__memrange_" + std::to_string(awst::NameGen::next("MemoryHelpers.range"));
	auto length = awst::makeEvalOnce(offsetToUint64(std::move(_length), _loc), _loc);
	// A subroutine evaluates all arguments. Preserve the EVM zero-length rule
	// by skipping even the checked narrowing of an unused, possibly huge offset.
	auto offset = awst::makeConditional(awst::makeNumericCompare(length, awst::NumericComparison::Eq,
		awst::makeZero(_loc), _loc), awst::makeZero(_loc),
		offsetToUint64(std::move(_offset), _loc), awst::WType::uint64Type(), _loc);
	auto value = awst::makeSubroutineCall(
		awst::SubroutineID{memoryBufferSubroutine(m_typeMapper, false, _loc, true)},
		awst::WType::bytesType(), _loc);
	awst::pushCallArg(value->args, std::move(offset));
	awst::pushCallArg(value->args, std::move(length));
	auto result = awst::makeVarExpression(name, awst::WType::bytesType(), _loc);
	_out.push_back(awst::makeAssignmentStatement(result, std::move(value), _loc));
	return result;
}

void AssemblyBuilder::writeMemRangeDyn(
	std::shared_ptr<awst::Expression> _offset,
	std::shared_ptr<awst::Expression> _value,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out)
{
	auto bytes = awst::makeVarExpression("__memwrite_" + std::to_string(
		awst::NameGen::next("MemoryHelpers.writeRange")), awst::WType::bytesType(), _loc);
	_out.push_back(awst::makeAssignmentStatement(bytes, std::move(_value), _loc));
	auto offset = awst::makeConditional(awst::makeNumericCompare(awst::makeLen(bytes, _loc),
		awst::NumericComparison::Eq, awst::makeZero(_loc), _loc), awst::makeZero(_loc),
		offsetToUint64(std::move(_offset), _loc), awst::WType::uint64Type(), _loc);
	auto call = awst::makeSubroutineCall(
		awst::SubroutineID{memoryBufferSubroutine(m_typeMapper, true, _loc, true)},
		awst::WType::voidType(), _loc);
	awst::pushCallArg(call->args, std::move(offset));
	awst::pushCallArg(call->args, bytes);
	_out.push_back(awst::makeExpressionStatement(std::move(call), _loc));
}


} // namespace puyasol::builder
