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
	return awst::makeAsBiguint(
		awst::makeExtract3(memoryVar(_loc),
			awst::makeIntegerConstant(_offset, _loc),
			awst::makeIntegerConstant("32", _loc), _loc), _loc);
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
	uint64_t byteOffset = _baseOffset + static_cast<uint64_t>(_startSlot) * 0x20;
	uint64_t byteLen = static_cast<uint64_t>(_count) * 0x20;
	return awst::makeExtract3(memoryVar(_loc),
		awst::makeIntegerConstant(byteOffset, _loc),
		awst::makeIntegerConstant(byteLen, _loc), _loc);
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
		auto cond = awst::makeConditional(std::move(_result),
			awst::makeBiguintConstant("1", _loc), awst::makeBiguintConstant("0", _loc),
			awst::WType::biguintType(), _loc);
		assignMemoryVar(awst::makeReplace3(memoryVar(_loc),
			awst::makeIntegerConstant(_outputOffset, _loc),
			padTo32Bytes(std::move(cond), _loc), _loc), _loc, _out);
		return;
	}

	if (_outputSlots == 1)
	{
		std::shared_ptr<awst::Expression> storeVal = std::move(_result);
		if (storeVal->wtype == awst::WType::bytesType())
			storeVal = awst::makeAsBiguint(std::move(storeVal), _loc);
		assignMemoryVar(awst::makeReplace3(memoryVar(_loc),
			awst::makeIntegerConstant(_outputOffset, _loc),
			padTo32Bytes(std::move(storeVal), _loc), _loc), _loc, _out);
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
		assignMemoryVar(awst::makeReplace3(memoryVar(_loc),
			awst::makeIntegerConstant(outOff, _loc), std::move(extractSlot), _loc), _loc, _out);
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

	if (_isBoolResult)
	{
		auto one = awst::makeBiguintConstant("1", _loc);
		auto zero = awst::makeBiguintConstant("0", _loc);
		auto cond = awst::makeConditional(
			std::move(_result), std::move(one), std::move(zero),
			awst::WType::biguintType(), _loc);
		auto padded = padTo32Bytes(std::move(cond), _loc);

		auto replace = awst::makeReplace3(memoryVar(_loc), std::move(baseOff), std::move(padded), _loc);
		assignMemoryVar(std::move(replace), _loc, _out);
		return;
	}

	if (_outputSlots == 1)
	{
		std::shared_ptr<awst::Expression> storeVal = std::move(_result);
		if (storeVal->wtype == awst::WType::bytesType())
			storeVal = awst::makeAsBiguint(std::move(storeVal), _loc);
		auto padded = padTo32Bytes(std::move(storeVal), _loc);

		auto replace = awst::makeReplace3(memoryVar(_loc), std::move(baseOff), std::move(padded), _loc);
		assignMemoryVar(std::move(replace), _loc, _out);
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

		auto replace = awst::makeReplace3(memoryVar(_loc), std::move(outOff), std::move(extractSlot), _loc);
		assignMemoryVar(std::move(replace), _loc, _out);
	}
}

// ─── Unified precompile dispatch ────────────────────────────────────────────


} // namespace puyasol::builder
