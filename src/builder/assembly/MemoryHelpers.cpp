/// @file MemoryHelpers.cpp
/// Memory helper functions: readMemSlot, padTo32Bytes, concatSlots, storeResultToMemory.
/// All operations work on the __evm_memory bytes blob backed by scratch slots.

#include "builder/assembly/AssemblyBuilder.h"
#include "Logger.h"

#include <sstream>

namespace puyasol::builder
{

std::shared_ptr<awst::Expression> AssemblyBuilder::readMemSlot(
	uint64_t _offset,
	awst::SourceLocation const& _loc
)
{
	// Read 32 bytes from the memory blob at a constant offset.
	// extract3(__evm_memory, offset, 32) → cast to biguint
	auto offsetConst = awst::makeIntegerConstant(std::to_string(_offset), _loc);

	auto len32 = awst::makeIntegerConstant("32", _loc);

	auto extract = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
	extract->stackArgs.push_back(memoryVar(_loc));
	extract->stackArgs.push_back(std::move(offsetConst));
	extract->stackArgs.push_back(std::move(len32));

	auto cast = awst::makeReinterpretCast(std::move(extract), awst::WType::biguintType(), _loc);
	return cast;
}

std::shared_ptr<awst::Expression> AssemblyBuilder::padTo32Bytes(
	std::shared_ptr<awst::Expression> _expr,
	awst::SourceLocation const& _loc
)
{
	auto cast = awst::makeReinterpretCast(std::move(_expr), awst::WType::bytesType(), _loc);

	auto zeroBytes = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), _loc);
	auto sz = awst::makeIntegerConstant("32", _loc);
	zeroBytes->stackArgs.push_back(sz);

	auto concatPad = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), _loc);
	concatPad->stackArgs.push_back(std::move(zeroBytes));
	concatPad->stackArgs.push_back(std::move(cast));

	auto lenCall = awst::makeIntrinsicCall("len", awst::WType::uint64Type(), _loc);
	lenCall->stackArgs.push_back(concatPad);

	auto n32 = awst::makeIntegerConstant("32", _loc);

	auto startOff = awst::makeIntrinsicCall("-", awst::WType::uint64Type(), _loc);
	startOff->stackArgs.push_back(std::move(lenCall));
	startOff->stackArgs.push_back(n32);

	auto extract = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
	extract->stackArgs.push_back(concatPad);
	extract->stackArgs.push_back(std::move(startOff));
	auto n32e = awst::makeIntegerConstant("32", _loc);
	extract->stackArgs.push_back(n32e);

	return extract;
}

std::shared_ptr<awst::Expression> AssemblyBuilder::concatSlots(
	uint64_t _baseOffset, int _startSlot, int _count,
	awst::SourceLocation const& _loc
)
{
	// With the blob model, this is a single extract3 instead of N reads + pads + concats.
	// extract3(__evm_memory, baseOffset + startSlot*32, count*32)
	uint64_t byteOffset = _baseOffset + static_cast<uint64_t>(_startSlot) * 0x20;
	uint64_t byteLen = static_cast<uint64_t>(_count) * 0x20;

	auto offsetConst = awst::makeIntegerConstant(std::to_string(byteOffset), _loc);

	auto lenConst = awst::makeIntegerConstant(std::to_string(byteLen), _loc);

	auto extract = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
	extract->stackArgs.push_back(memoryVar(_loc));
	extract->stackArgs.push_back(std::move(offsetConst));
	extract->stackArgs.push_back(std::move(lenConst));

	return extract;
}

void AssemblyBuilder::storeResultToMemory(
	std::shared_ptr<awst::Expression> _result,
	uint64_t _outputOffset, int _outputSlots,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out,
	bool _isBoolResult
)
{
	if (_isBoolResult)
	{
		// Bool result → convert to 32-byte biguint (1 or 0), store at offset
		auto one = awst::makeIntegerConstant("1", _loc, awst::WType::biguintType());
		auto zero = awst::makeIntegerConstant("0", _loc, awst::WType::biguintType());

		auto cond = std::make_shared<awst::ConditionalExpression>();
		cond->sourceLocation = _loc;
		cond->wtype = awst::WType::biguintType();
		cond->condition = std::move(_result);
		cond->trueExpr = std::move(one);
		cond->falseExpr = std::move(zero);

		auto padded = padTo32Bytes(std::move(cond), _loc);

		auto offsetConst = awst::makeIntegerConstant(std::to_string(_outputOffset), _loc);

		auto replace = awst::makeIntrinsicCall("replace3", awst::WType::bytesType(), _loc);
		replace->stackArgs.push_back(memoryVar(_loc));
		replace->stackArgs.push_back(std::move(offsetConst));
		replace->stackArgs.push_back(std::move(padded));

		assignMemoryVar(std::move(replace), _loc, _out);
		return;
	}

	if (_outputSlots == 1)
	{
		// Single 32-byte slot: pad result and write to blob
		std::shared_ptr<awst::Expression> storeVal = std::move(_result);
		if (storeVal->wtype == awst::WType::bytesType())
		{
			auto cast = awst::makeReinterpretCast(std::move(storeVal), awst::WType::biguintType(), _loc);
			storeVal = std::move(cast);
		}

		auto padded = padTo32Bytes(std::move(storeVal), _loc);

		auto offsetConst = awst::makeIntegerConstant(std::to_string(_outputOffset), _loc);

		auto replace = awst::makeIntrinsicCall("replace3", awst::WType::bytesType(), _loc);
		replace->stackArgs.push_back(memoryVar(_loc));
		replace->stackArgs.push_back(std::move(offsetConst));
		replace->stackArgs.push_back(std::move(padded));

		assignMemoryVar(std::move(replace), _loc, _out);
		return;
	}

	// Multi-slot: store result bytes to a temp var, then replace the whole region.
	// If result is already bytes of the right length, write directly.
	// Otherwise store to temp, then extract chunks.
	std::string resultVar = "__precompile_result";
	m_locals[resultVar] = awst::WType::bytesType();

	auto resultTarget = awst::makeVarExpression(resultVar, awst::WType::bytesType(), _loc);

	auto assignResult = awst::makeAssignmentStatement(resultTarget, std::move(_result), _loc);
	_out.push_back(std::move(assignResult));

	// Write each 32-byte chunk from the result into the blob
	for (int i = 0; i < _outputSlots; ++i)
	{
		uint64_t outOff = _outputOffset + static_cast<uint64_t>(i) * 0x20;

		auto resultRead = awst::makeVarExpression(resultVar, awst::WType::bytesType(), _loc);

		// extract3(result, i*32, 32)
		auto slotStart = awst::makeIntegerConstant(std::to_string(i * 32), _loc);

		auto slotLen = awst::makeIntegerConstant("32", _loc);

		auto extractSlot = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
		extractSlot->stackArgs.push_back(resultRead);
		extractSlot->stackArgs.push_back(slotStart);
		extractSlot->stackArgs.push_back(slotLen);

		// replace3(__evm_memory, outOff, chunk)
		auto offsetConst = awst::makeIntegerConstant(std::to_string(outOff), _loc);

		auto replace = awst::makeIntrinsicCall("replace3", awst::WType::bytesType(), _loc);
		replace->stackArgs.push_back(memoryVar(_loc));
		replace->stackArgs.push_back(std::move(offsetConst));
		replace->stackArgs.push_back(std::move(extractSlot));

		assignMemoryVar(std::move(replace), _loc, _out);
	}
}

// ─── Runtime-offset variants ────────────────────────────────────────────
//
// Used when a Yul precompile staticcall has dynamic memory offsets/sizes
// (e.g. honk's `add(free, 0x40)` where `free = mload(0x40)` is variable).
// These compute `extract3(__evm_memory, baseOffset + slotOffset, count*32)`
// at runtime instead of baking constants into the AWST.

std::shared_ptr<awst::Expression> AssemblyBuilder::concatSlotsRT(
	std::shared_ptr<awst::Expression> _baseOffset, int _startSlot, int _count,
	awst::SourceLocation const& _loc
)
{
	using O = awst::UInt64BinaryOperator;
	auto base = offsetToUint64(std::move(_baseOffset), _loc);
	auto offsetExpr = (_startSlot != 0)
		? std::shared_ptr<awst::Expression>(awst::makeUInt64BinOp(
			std::move(base), O::Add,
			awst::makeIntegerConstant(std::to_string(_startSlot * 0x20), _loc),
			_loc))
		: base;

	auto lenConst = awst::makeIntegerConstant(std::to_string(_count * 0x20), _loc);

	auto extract = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
	extract->stackArgs.push_back(memoryVar(_loc));
	extract->stackArgs.push_back(std::move(offsetExpr));
	extract->stackArgs.push_back(std::move(lenConst));

	return extract;
}

void AssemblyBuilder::storeResultToMemoryRT(
	std::shared_ptr<awst::Expression> _result,
	std::shared_ptr<awst::Expression> _outputOffset, int _outputSlots,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out,
	bool _isBoolResult
)
{
	using O = awst::UInt64BinaryOperator;
	auto baseOff = offsetToUint64(std::move(_outputOffset), _loc);

	if (_isBoolResult)
	{
		auto one = awst::makeIntegerConstant("1", _loc, awst::WType::biguintType());
		auto zero = awst::makeIntegerConstant("0", _loc, awst::WType::biguintType());
		auto cond = std::make_shared<awst::ConditionalExpression>();
		cond->sourceLocation = _loc;
		cond->wtype = awst::WType::biguintType();
		cond->condition = std::move(_result);
		cond->trueExpr = std::move(one);
		cond->falseExpr = std::move(zero);
		auto padded = padTo32Bytes(std::move(cond), _loc);

		auto replace = awst::makeIntrinsicCall("replace3", awst::WType::bytesType(), _loc);
		replace->stackArgs.push_back(memoryVar(_loc));
		replace->stackArgs.push_back(std::move(baseOff));
		replace->stackArgs.push_back(std::move(padded));
		assignMemoryVar(std::move(replace), _loc, _out);
		return;
	}

	if (_outputSlots == 1)
	{
		std::shared_ptr<awst::Expression> storeVal = std::move(_result);
		if (storeVal->wtype == awst::WType::bytesType())
			storeVal = awst::makeReinterpretCast(std::move(storeVal), awst::WType::biguintType(), _loc);
		auto padded = padTo32Bytes(std::move(storeVal), _loc);

		auto replace = awst::makeIntrinsicCall("replace3", awst::WType::bytesType(), _loc);
		replace->stackArgs.push_back(memoryVar(_loc));
		replace->stackArgs.push_back(std::move(baseOff));
		replace->stackArgs.push_back(std::move(padded));
		assignMemoryVar(std::move(replace), _loc, _out);
		return;
	}

	// Multi-slot: stash result bytes in a temp local, then write each
	// 32-byte chunk into the memory blob at runtime offsets.
	std::string resultVar = "__precompile_result";
	m_locals[resultVar] = awst::WType::bytesType();
	auto resultTarget = awst::makeVarExpression(resultVar, awst::WType::bytesType(), _loc);
	_out.push_back(awst::makeAssignmentStatement(resultTarget, std::move(_result), _loc));

	// Bind base offset to a local so we don't duplicate the offset
	// expression once per chunk.
	std::string offsetVar = "__precompile_outoff";
	m_locals[offsetVar] = awst::WType::uint64Type();
	_out.push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression(offsetVar, awst::WType::uint64Type(), _loc),
		std::move(baseOff), _loc));

	for (int i = 0; i < _outputSlots; ++i)
	{
		auto resultRead = awst::makeVarExpression(resultVar, awst::WType::bytesType(), _loc);
		auto slotStart = awst::makeIntegerConstant(std::to_string(i * 32), _loc);
		auto slotLen = awst::makeIntegerConstant("32", _loc);
		auto extractSlot = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
		extractSlot->stackArgs.push_back(resultRead);
		extractSlot->stackArgs.push_back(std::move(slotStart));
		extractSlot->stackArgs.push_back(std::move(slotLen));

		auto offBase = awst::makeVarExpression(offsetVar, awst::WType::uint64Type(), _loc);
		std::shared_ptr<awst::Expression> outOff = (i == 0)
			? offBase
			: std::shared_ptr<awst::Expression>(awst::makeUInt64BinOp(
				std::move(offBase), O::Add,
				awst::makeIntegerConstant(std::to_string(i * 32), _loc), _loc));

		auto replace = awst::makeIntrinsicCall("replace3", awst::WType::bytesType(), _loc);
		replace->stackArgs.push_back(memoryVar(_loc));
		replace->stackArgs.push_back(std::move(outOff));
		replace->stackArgs.push_back(std::move(extractSlot));
		assignMemoryVar(std::move(replace), _loc, _out);
	}
}

// ─── Unified precompile dispatch ────────────────────────────────────────────


} // namespace puyasol::builder
