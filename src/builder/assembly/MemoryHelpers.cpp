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
	auto offsetConst = std::make_shared<awst::IntegerConstant>();
	offsetConst->sourceLocation = _loc;
	offsetConst->wtype = awst::WType::uint64Type();
	offsetConst->value = std::to_string(_offset);

	auto len32 = std::make_shared<awst::IntegerConstant>();
	len32->sourceLocation = _loc;
	len32->wtype = awst::WType::uint64Type();
	len32->value = "32";

	auto extract = std::make_shared<awst::IntrinsicCall>();
	extract->sourceLocation = _loc;
	extract->wtype = awst::WType::bytesType();
	extract->opCode = "extract3";
	extract->stackArgs.push_back(memoryVar(_loc));
	extract->stackArgs.push_back(std::move(offsetConst));
	extract->stackArgs.push_back(std::move(len32));

	auto cast = std::make_shared<awst::ReinterpretCast>();
	cast->sourceLocation = _loc;
	cast->wtype = awst::WType::biguintType();
	cast->expr = std::move(extract);
	return cast;
}

std::shared_ptr<awst::Expression> AssemblyBuilder::padTo32Bytes(
	std::shared_ptr<awst::Expression> _expr,
	awst::SourceLocation const& _loc
)
{
	auto cast = std::make_shared<awst::ReinterpretCast>();
	cast->sourceLocation = _loc;
	cast->wtype = awst::WType::bytesType();
	cast->expr = std::move(_expr);

	auto zeroBytes = std::make_shared<awst::IntrinsicCall>();
	zeroBytes->sourceLocation = _loc;
	zeroBytes->wtype = awst::WType::bytesType();
	zeroBytes->opCode = "bzero";
	auto sz = std::make_shared<awst::IntegerConstant>();
	sz->sourceLocation = _loc;
	sz->wtype = awst::WType::uint64Type();
	sz->value = "32";
	zeroBytes->stackArgs.push_back(sz);

	auto concatPad = std::make_shared<awst::IntrinsicCall>();
	concatPad->sourceLocation = _loc;
	concatPad->wtype = awst::WType::bytesType();
	concatPad->opCode = "concat";
	concatPad->stackArgs.push_back(std::move(zeroBytes));
	concatPad->stackArgs.push_back(std::move(cast));

	auto lenCall = std::make_shared<awst::IntrinsicCall>();
	lenCall->sourceLocation = _loc;
	lenCall->wtype = awst::WType::uint64Type();
	lenCall->opCode = "len";
	lenCall->stackArgs.push_back(concatPad);

	auto n32 = std::make_shared<awst::IntegerConstant>();
	n32->sourceLocation = _loc;
	n32->wtype = awst::WType::uint64Type();
	n32->value = "32";

	auto startOff = std::make_shared<awst::IntrinsicCall>();
	startOff->sourceLocation = _loc;
	startOff->wtype = awst::WType::uint64Type();
	startOff->opCode = "-";
	startOff->stackArgs.push_back(std::move(lenCall));
	startOff->stackArgs.push_back(n32);

	auto extract = std::make_shared<awst::IntrinsicCall>();
	extract->sourceLocation = _loc;
	extract->wtype = awst::WType::bytesType();
	extract->opCode = "extract3";
	extract->stackArgs.push_back(concatPad);
	extract->stackArgs.push_back(std::move(startOff));
	auto n32e = std::make_shared<awst::IntegerConstant>();
	n32e->sourceLocation = _loc;
	n32e->wtype = awst::WType::uint64Type();
	n32e->value = "32";
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

	auto offsetConst = std::make_shared<awst::IntegerConstant>();
	offsetConst->sourceLocation = _loc;
	offsetConst->wtype = awst::WType::uint64Type();
	offsetConst->value = std::to_string(byteOffset);

	auto lenConst = std::make_shared<awst::IntegerConstant>();
	lenConst->sourceLocation = _loc;
	lenConst->wtype = awst::WType::uint64Type();
	lenConst->value = std::to_string(byteLen);

	auto extract = std::make_shared<awst::IntrinsicCall>();
	extract->sourceLocation = _loc;
	extract->wtype = awst::WType::bytesType();
	extract->opCode = "extract3";
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
		auto one = std::make_shared<awst::IntegerConstant>();
		one->sourceLocation = _loc;
		one->wtype = awst::WType::biguintType();
		one->value = "1";
		auto zero = std::make_shared<awst::IntegerConstant>();
		zero->sourceLocation = _loc;
		zero->wtype = awst::WType::biguintType();
		zero->value = "0";

		auto cond = std::make_shared<awst::ConditionalExpression>();
		cond->sourceLocation = _loc;
		cond->wtype = awst::WType::biguintType();
		cond->condition = std::move(_result);
		cond->trueExpr = std::move(one);
		cond->falseExpr = std::move(zero);

		auto padded = padTo32Bytes(std::move(cond), _loc);

		auto offsetConst = std::make_shared<awst::IntegerConstant>();
		offsetConst->sourceLocation = _loc;
		offsetConst->wtype = awst::WType::uint64Type();
		offsetConst->value = std::to_string(_outputOffset);

		auto replace = std::make_shared<awst::IntrinsicCall>();
		replace->sourceLocation = _loc;
		replace->wtype = awst::WType::bytesType();
		replace->opCode = "replace3";
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
			auto cast = std::make_shared<awst::ReinterpretCast>();
			cast->sourceLocation = _loc;
			cast->wtype = awst::WType::biguintType();
			cast->expr = std::move(storeVal);
			storeVal = std::move(cast);
		}

		auto padded = padTo32Bytes(std::move(storeVal), _loc);

		auto offsetConst = std::make_shared<awst::IntegerConstant>();
		offsetConst->sourceLocation = _loc;
		offsetConst->wtype = awst::WType::uint64Type();
		offsetConst->value = std::to_string(_outputOffset);

		auto replace = std::make_shared<awst::IntrinsicCall>();
		replace->sourceLocation = _loc;
		replace->wtype = awst::WType::bytesType();
		replace->opCode = "replace3";
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

	auto resultTarget = std::make_shared<awst::VarExpression>();
	resultTarget->sourceLocation = _loc;
	resultTarget->name = resultVar;
	resultTarget->wtype = awst::WType::bytesType();

	auto assignResult = std::make_shared<awst::AssignmentStatement>();
	assignResult->sourceLocation = _loc;
	assignResult->target = resultTarget;
	assignResult->value = std::move(_result);
	_out.push_back(std::move(assignResult));

	// Write each 32-byte chunk from the result into the blob
	for (int i = 0; i < _outputSlots; ++i)
	{
		uint64_t outOff = _outputOffset + static_cast<uint64_t>(i) * 0x20;

		auto resultRead = std::make_shared<awst::VarExpression>();
		resultRead->sourceLocation = _loc;
		resultRead->name = resultVar;
		resultRead->wtype = awst::WType::bytesType();

		// extract3(result, i*32, 32)
		auto slotStart = std::make_shared<awst::IntegerConstant>();
		slotStart->sourceLocation = _loc;
		slotStart->wtype = awst::WType::uint64Type();
		slotStart->value = std::to_string(i * 32);

		auto slotLen = std::make_shared<awst::IntegerConstant>();
		slotLen->sourceLocation = _loc;
		slotLen->wtype = awst::WType::uint64Type();
		slotLen->value = "32";

		auto extractSlot = std::make_shared<awst::IntrinsicCall>();
		extractSlot->sourceLocation = _loc;
		extractSlot->wtype = awst::WType::bytesType();
		extractSlot->opCode = "extract3";
		extractSlot->stackArgs.push_back(resultRead);
		extractSlot->stackArgs.push_back(slotStart);
		extractSlot->stackArgs.push_back(slotLen);

		// replace3(__evm_memory, outOff, chunk)
		auto offsetConst = std::make_shared<awst::IntegerConstant>();
		offsetConst->sourceLocation = _loc;
		offsetConst->wtype = awst::WType::uint64Type();
		offsetConst->value = std::to_string(outOff);

		auto replace = std::make_shared<awst::IntrinsicCall>();
		replace->sourceLocation = _loc;
		replace->wtype = awst::WType::bytesType();
		replace->opCode = "replace3";
		replace->stackArgs.push_back(memoryVar(_loc));
		replace->stackArgs.push_back(std::move(offsetConst));
		replace->stackArgs.push_back(std::move(extractSlot));

		assignMemoryVar(std::move(replace), _loc, _out);
	}
}

// ─── Unified precompile dispatch ────────────────────────────────────────────


} // namespace puyasol::builder
