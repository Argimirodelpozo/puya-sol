/// @file MemoryHelpers.cpp
/// readMemSlot, padTo32Bytes, concatSlots, storeResultToMemory — scratch-slot memory helpers.

#include "builder/assembly/AssemblyBuilder.h"

#include <sstream>

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
		readMemWordDirect(scratchLayout(), awst::makeIntegerConstant(_offset, _loc), _loc), _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::padTo32Bytes(
	std::shared_ptr<awst::Expression> _expr,
	awst::SourceLocation const& _loc
)
{
	auto cast = awst::makeAsBytes(std::move(_expr), _loc);
	auto concatPad = awst::makeLeftPad(std::move(cast), 32, _loc);
	return awst::makeExtractLastN(std::move(concatPad), 32, _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::concatSlots(
	uint64_t _baseOffset, int _startSlot, int _count,
	awst::SourceLocation const& _loc
)
{
	// Slot-routed range read (M7): words straddling SLOT_SIZE are handled.
	uint64_t byteOffset = _baseOffset + static_cast<uint64_t>(_startSlot) * 0x20;
	int byteLen = _count * 0x20;
	return readMemRangeDirect(scratchLayout(),
		awst::makeIntegerConstant(byteOffset, _loc), byteLen, _loc);
}

void AssemblyBuilder::storeResultToMemory(
	std::shared_ptr<awst::Expression> _result,
	uint64_t _outputOffset, int _outputSlots,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out,
	bool _isBoolResult
)
{
	// All writes slot-routed via writeMemWordDirect (M7): output offsets
	// ≥ SLOT_SIZE land in the right scratch slot.
	if (_isBoolResult)
	{
		auto cond = awst::makeConditional(std::move(_result),
			awst::makeBiguintConstant("1", _loc), awst::makeBiguintConstant("0", _loc),
			awst::WType::biguintType(), _loc);
		writeMemWordDirect(scratchLayout(), awst::makeIntegerConstant(_outputOffset, _loc),
			padTo32Bytes(std::move(cond), _loc), _loc, _out);
		return;
	}

	if (_outputSlots == 1)
	{
		std::shared_ptr<awst::Expression> storeVal = std::move(_result);
		if (storeVal->wtype == awst::WType::bytesType())
			storeVal = awst::makeAsBiguint(std::move(storeVal), _loc);
		writeMemWordDirect(scratchLayout(), awst::makeIntegerConstant(_outputOffset, _loc),
			padTo32Bytes(std::move(storeVal), _loc), _loc, _out);
		return;
	}

	// Multi-slot: stash result in temp, then write each 32-byte chunk.
	std::string resultVar = "__precompile_result";
	m_locals[resultVar] = awst::WType::bytesType();

	_out.push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression(resultVar, awst::WType::bytesType(), _loc),
		std::move(_result), _loc));

	for (int i = 0; i < _outputSlots; ++i)
	{
		uint64_t outOff = _outputOffset + static_cast<uint64_t>(i) * 0x20;
		auto extractSlot = awst::makeExtract3(
			awst::makeVarExpression(resultVar, awst::WType::bytesType(), _loc),
			awst::makeIntegerConstant(i * 32, _loc),
			awst::makeIntegerConstant("32", _loc), _loc);
		writeMemWordDirect(scratchLayout(), awst::makeIntegerConstant(outOff, _loc),
			std::move(extractSlot), _loc, _out);
	}
}

// ─── Runtime-offset variants ────────────────────────────────────────────
// Used when precompile staticcall has dynamic offsets (e.g. honk: add(free, 0x40)).

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
			awst::makeIntegerConstant(_startSlot * 0x20, _loc),
			_loc))
		: base;
	// Materialize once: EC-precompile free-pointer base otherwise re-read _count
	// times (makeEvalOnce = OperandPlan primitive; skipped entirely for a single
	// reference).
	if (_count > 1)
		offsetExpr = awst::makeEvalOnce(std::move(offsetExpr), _loc);

	// readMemWordDyn: slot-0 vs scratch conditional needed because EC precompile inputs
	// live at the runtime FMP; in a split piece slot-0 is in local while slot 1+ is in
	// scratch — loads-only would mishandle slot 0. Words may straddle SLOT_SIZE.
	std::shared_ptr<awst::Expression> acc;
	for (int i = 0; i < _count; ++i)
	{
		std::shared_ptr<awst::Expression> wordOff = (i == 0)
			? offsetExpr
			: awst::makeUInt64BinOp(offsetExpr, O::Add,
				awst::makeIntegerConstant(static_cast<uint64_t>(i * 0x20), _loc), _loc);
		auto word = readMemWordDyn(std::move(wordOff), _loc);
		acc = acc ? awst::makeConcat(std::move(acc), std::move(word), _loc) : std::move(word);
	}
	return acc;
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

	// All writes slot-routed via writeMemWordDyn (M7): runtime output offsets
	// ≥ SLOT_SIZE land in the right scratch slot.
	if (_isBoolResult)
	{
		auto one = awst::makeBiguintConstant("1", _loc);
		auto zero = awst::makeBiguintConstant("0", _loc);
		auto cond = awst::makeConditional(
			std::move(_result), std::move(one), std::move(zero),
			awst::WType::biguintType(), _loc);
		auto padded = padTo32Bytes(std::move(cond), _loc);
		writeMemWordDyn(std::move(baseOff), std::move(padded), _loc, _out);
		return;
	}

	if (_outputSlots == 1)
	{
		std::shared_ptr<awst::Expression> storeVal = std::move(_result);
		if (storeVal->wtype == awst::WType::bytesType())
			storeVal = awst::makeAsBiguint(std::move(storeVal), _loc);
		auto padded = padTo32Bytes(std::move(storeVal), _loc);
		writeMemWordDyn(std::move(baseOff), std::move(padded), _loc, _out);
		return;
	}

	// Multi-slot: stash result and base offset in temps to avoid per-chunk duplication.
	std::string resultVar = "__precompile_result";
	m_locals[resultVar] = awst::WType::bytesType();
	_out.push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression(resultVar, awst::WType::bytesType(), _loc),
		std::move(_result), _loc));

	std::string offsetVar = "__precompile_outoff";
	m_locals[offsetVar] = awst::WType::uint64Type();
	_out.push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression(offsetVar, awst::WType::uint64Type(), _loc),
		std::move(baseOff), _loc));

	for (int i = 0; i < _outputSlots; ++i)
	{
		auto resultRead = awst::makeVarExpression(resultVar, awst::WType::bytesType(), _loc);
		auto slotStart = awst::makeIntegerConstant(i * 32, _loc);
		auto slotLen = awst::makeIntegerConstant("32", _loc);
		auto extractSlot = awst::makeExtract3(resultRead, std::move(slotStart), std::move(slotLen), _loc);
		auto offBase = awst::makeVarExpression(offsetVar, awst::WType::uint64Type(), _loc);
		std::shared_ptr<awst::Expression> outOff = (i == 0)
			? offBase
			: std::shared_ptr<awst::Expression>(awst::makeUInt64BinOp(
				std::move(offBase), O::Add,
				awst::makeIntegerConstant(i * 32, _loc), _loc));
		writeMemWordDyn(std::move(outOff), std::move(extractSlot), _loc, _out);
	}
}

// ─── Unified precompile dispatch ────────────────────────────────────────────


} // namespace puyasol::builder
