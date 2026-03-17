/// @file MemoryHelpers.cpp
/// Memory helper functions: readMemSlot, padTo32Bytes, concatSlots, storeResultToMemory.

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
	auto it = m_memoryMap.find(_offset);
	if (it != m_memoryMap.end())
	{
		auto const& slot = it->second;
		if (slot.isParam)
		{
			auto base = std::make_shared<awst::VarExpression>();
			base->sourceLocation = _loc;
			base->name = slot.varName;
			base->wtype = m_arrayParamType;
			auto index = std::make_shared<awst::IntegerConstant>();
			index->sourceLocation = _loc;
			index->wtype = awst::WType::uint64Type();
			index->value = std::to_string(slot.paramIndex);
			auto indexExpr = std::make_shared<awst::IndexExpression>();
			indexExpr->sourceLocation = _loc;
			indexExpr->wtype = awst::WType::biguintType();
			indexExpr->base = std::move(base);
			indexExpr->index = std::move(index);
			return indexExpr;
		}
		else
		{
			auto var = std::make_shared<awst::VarExpression>();
			var->sourceLocation = _loc;
			var->name = slot.varName;
			var->wtype = awst::WType::biguintType();
			return var;
		}
	}
	Logger::instance().warning(
		"precompile input at offset 0x" +
		([&] { std::ostringstream oss; oss << std::hex << _offset; return oss.str(); })() +
		" not found, using zero", _loc
	);
	auto zero = std::make_shared<awst::IntegerConstant>();
	zero->sourceLocation = _loc;
	zero->wtype = awst::WType::biguintType();
	zero->value = "0";
	return zero;
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
	std::shared_ptr<awst::Expression> result;
	for (int i = 0; i < _count; ++i)
	{
		uint64_t off = _baseOffset + static_cast<uint64_t>(_startSlot + i) * 0x20;
		auto slotBytes = padTo32Bytes(readMemSlot(off, _loc), _loc);
		if (!result)
			result = std::move(slotBytes);
		else
		{
			auto concat = std::make_shared<awst::IntrinsicCall>();
			concat->sourceLocation = _loc;
			concat->wtype = awst::WType::bytesType();
			concat->opCode = "concat";
			concat->stackArgs.push_back(std::move(result));
			concat->stackArgs.push_back(std::move(slotBytes));
			result = std::move(concat);
		}
	}
	return result;
}

std::string AssemblyBuilder::getOrCreateMemoryVar(
	uint64_t _offset,
	awst::SourceLocation const& _loc
)
{
	(void)_loc;
	auto it = m_memoryMap.find(_offset);
	if (it != m_memoryMap.end() && !it->second.isParam)
		return it->second.varName;

	std::string varName = "mem_0x" + ([&] {
		std::ostringstream oss;
		oss << std::hex << _offset;
		return oss.str();
	})();
	MemorySlot slot;
	slot.varName = varName;
	m_memoryMap[_offset] = slot;
	return varName;
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
		// Bool result → store as biguint (1 or 0) at outputOffset
		std::string varName = getOrCreateMemoryVar(_outputOffset, _loc);
		m_locals[varName] = awst::WType::biguintType();

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

		auto target = std::make_shared<awst::VarExpression>();
		target->sourceLocation = _loc;
		target->name = varName;
		target->wtype = awst::WType::biguintType();

		auto assign = std::make_shared<awst::AssignmentStatement>();
		assign->sourceLocation = _loc;
		assign->target = std::move(target);
		assign->value = std::move(cond);
		_out.push_back(std::move(assign));
		return;
	}

	if (_outputSlots == 1)
	{
		// Single slot: result is already biguint or bytes — store directly
		std::string varName = getOrCreateMemoryVar(_outputOffset, _loc);
		m_locals[varName] = awst::WType::biguintType();

		// If result is bytes, cast to biguint
		std::shared_ptr<awst::Expression> storeVal = std::move(_result);
		if (storeVal->wtype == awst::WType::bytesType())
		{
			auto cast = std::make_shared<awst::ReinterpretCast>();
			cast->sourceLocation = _loc;
			cast->wtype = awst::WType::biguintType();
			cast->expr = std::move(storeVal);
			storeVal = std::move(cast);
		}

		auto target = std::make_shared<awst::VarExpression>();
		target->sourceLocation = _loc;
		target->name = varName;
		target->wtype = awst::WType::biguintType();

		auto assign = std::make_shared<awst::AssignmentStatement>();
		assign->sourceLocation = _loc;
		assign->target = std::move(target);
		assign->value = std::move(storeVal);
		_out.push_back(std::move(assign));
		return;
	}

	// Multi-slot: store result bytes in a temporary, then extract 32-byte chunks
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

	for (int i = 0; i < _outputSlots; ++i)
	{
		uint64_t outOff = _outputOffset + static_cast<uint64_t>(i) * 0x20;
		std::string varName = getOrCreateMemoryVar(outOff, _loc);
		m_locals[varName] = awst::WType::biguintType();

		auto resultRead = std::make_shared<awst::VarExpression>();
		resultRead->sourceLocation = _loc;
		resultRead->name = resultVar;
		resultRead->wtype = awst::WType::bytesType();

		auto extractSlot = std::make_shared<awst::IntrinsicCall>();
		extractSlot->sourceLocation = _loc;
		extractSlot->wtype = awst::WType::bytesType();
		extractSlot->opCode = "extract3";
		extractSlot->stackArgs.push_back(resultRead);

		auto slotStart = std::make_shared<awst::IntegerConstant>();
		slotStart->sourceLocation = _loc;
		slotStart->wtype = awst::WType::uint64Type();
		slotStart->value = std::to_string(i * 32);
		extractSlot->stackArgs.push_back(slotStart);

		auto slotLen = std::make_shared<awst::IntegerConstant>();
		slotLen->sourceLocation = _loc;
		slotLen->wtype = awst::WType::uint64Type();
		slotLen->value = "32";
		extractSlot->stackArgs.push_back(slotLen);

		auto castSlot = std::make_shared<awst::ReinterpretCast>();
		castSlot->sourceLocation = _loc;
		castSlot->wtype = awst::WType::biguintType();
		castSlot->expr = std::move(extractSlot);

		auto target = std::make_shared<awst::VarExpression>();
		target->sourceLocation = _loc;
		target->name = varName;
		target->wtype = awst::WType::biguintType();

		auto assign = std::make_shared<awst::AssignmentStatement>();
		assign->sourceLocation = _loc;
		assign->target = std::move(target);
		assign->value = std::move(castSlot);
		_out.push_back(std::move(assign));
	}
}

// ─── Unified precompile dispatch ────────────────────────────────────────────


} // namespace puyasol::builder
