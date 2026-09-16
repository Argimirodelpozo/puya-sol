/// @file SlotHandleAccess.cpp
/// See SlotHandleAccess.h.

#include "builder/storage/slot/SlotHandleAccess.h"
#include "builder/types/EncodedSize.h"
#include "awst/NameGen.h"

#include <libsolidity/ast/AST.h>

#include <functional>

namespace puyasol::builder
{

namespace
{
std::shared_ptr<awst::Expression> biguintConst(uint64_t v, awst::SourceLocation const& loc)
{
	return awst::makeIntegerConstant(std::to_string(v), loc, awst::WType::biguintType());
}
} // namespace

std::shared_ptr<awst::Expression> SlotHandleAccess::boundsCheckIndex(
	std::vector<std::shared_ptr<awst::Statement>>& _preStmts,
	std::shared_ptr<awst::Expression> _idx,
	solidity::frontend::ArrayType const* _arrType,
	awst::SourceLocation const& _loc)
{
	if (!_idx || !_arrType || _arrType->isDynamicallySized())
		return _idx;
	// Pin to a TEMP (not SingleEvaluation): the assert lands in its own
	// pre-STATEMENT while the index is consumed by a later expression — a
	// temp var is the established cross-statement idiom
	// (checkedIndexToUint64, the mapping-chain bounds check); vars and
	// constants are re-creatable and skip the temp.
	if (!dynamic_cast<awst::VarExpression const*>(_idx.get())
		&& !dynamic_cast<awst::IntegerConstant const*>(_idx.get()))
	{
		std::string nm = "__shb_idx_"
			+ std::to_string(awst::NameGen::next("SlotHandleAccess.boundsIdx"));
		auto const* idxWt = _idx->wtype; // read BEFORE the move (arg eval order)
		_preStmts.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(nm, idxWt, _loc), std::move(_idx), _loc));
		_idx = awst::makeVarExpression(nm, idxWt, _loc);
	}
	auto idxRef = [&]() -> std::shared_ptr<awst::Expression> {
		if (auto const* ve = dynamic_cast<awst::VarExpression const*>(_idx.get()))
			return awst::makeVarExpression(ve->name, ve->wtype, _loc);
		auto const* ic = static_cast<awst::IntegerConstant const*>(_idx.get());
		return awst::makeIntegerConstant(ic->value, _loc, ic->wtype);
	};
	auto bound = awst::makeIntegerConstant(
		_arrType->length().str(), _loc, _idx->wtype);
	auto cmp = awst::makeNumericCompare(
		idxRef(), awst::NumericComparison::Lt, std::move(bound), _loc);
	_preStmts.push_back(awst::makeExpressionStatement(
		awst::makeAssert(std::move(cmp), _loc, "array index out of bounds"), _loc));
	return _idx;
}

SlotHandleAccess::ElemLayout SlotHandleAccess::layoutFor(
	solidity::frontend::Type const* _elemType)
{
	ElemLayout l;
	if (!_elemType)
		return l;
	auto slots = _elemType->storageSize();
	unsigned bytes = _elemType->storageBytes();
	if (slots > 1 || bytes == 32)
	{
		l.strideSlots = slots;
		l.perSlot = 1;
		l.size = 32;
	}
	else
	{
		l.strideSlots = 1;
		l.size = bytes;
		l.perSlot = 32 / bytes;
	}
	return l;
}

std::shared_ptr<awst::Expression> SlotHandleAccess::elemSlot(
	std::shared_ptr<awst::Expression> _base,
	std::shared_ptr<awst::Expression> _idx,
	ElemLayout const& _l,
	awst::SourceLocation const& _loc)
{
	std::shared_ptr<awst::Expression> delta;
	if (_l.perSlot > 1)
		delta = awst::makeBigUIntBinOp(std::move(_idx),
			awst::BigUIntBinaryOperator::FloorDiv, biguintConst(_l.perSlot, _loc), _loc);
	else if (_l.strideSlots > 1)
		delta = awst::makeBigUIntBinOp(std::move(_idx),
			awst::BigUIntBinaryOperator::Mult,
			awst::makeIntegerConstant(_l.strideSlots.str(), _loc, awst::WType::biguintType()), _loc);
	else
		delta = std::move(_idx);
	return awst::makeBigUIntBinOp(std::move(_base),
		awst::BigUIntBinaryOperator::Add, std::move(delta), _loc);
}

std::shared_ptr<awst::Expression> SlotHandleAccess::readSlot(
	std::shared_ptr<awst::Expression> _slot, awst::SourceLocation const& _loc)
{
	auto call = awst::makeSubroutineCall(
		awst::SubroutineID{"__puyasol___storage_read"}, awst::WType::biguintType(), _loc);
	awst::pushCallArg(call->args, "__slot", std::move(_slot));
	return call;
}

std::shared_ptr<awst::Statement> SlotHandleAccess::writeSlot(
	std::shared_ptr<awst::Expression> _slot,
	std::shared_ptr<awst::Expression> _valueBiguint,
	awst::SourceLocation const& _loc)
{
	auto call = awst::makeSubroutineCall(
		awst::SubroutineID{"__puyasol___storage_write"}, awst::WType::voidType(), _loc);
	awst::pushCallArg(call->args, "__slot", std::move(_slot));
	awst::pushCallArg(call->args, "__value", std::move(_valueBiguint));
	return awst::makeExpressionStatement(std::move(call), _loc);
}

bool SlotHandleAccess::forEachIndex(solidity::u256 const& _count,
	std::vector<std::shared_ptr<awst::Statement>>& _out,
	awst::SourceLocation const& _loc,
	std::function<bool(std::shared_ptr<awst::Expression>,
		std::vector<std::shared_ptr<awst::Statement>>&)> const& _emit)
{
	if (_count <= 4)
	{
		for (unsigned i = 0; i < static_cast<unsigned>(_count); ++i)
			if (!_emit(biguintConst(i, _loc), _out)) return false;
		return true;
	}
	auto index = awst::makeVarExpression("__slot_index_"
		+ std::to_string(awst::NameGen::next("SlotHandleAccess.index")), awst::WType::biguintType(), _loc);
	auto body = awst::makeBlock(_loc);
	if (!_emit(index, body->body)) return false;
	body->body.push_back(awst::makeAssignmentStatement(index,
		awst::makeBigUIntBinOp(index, awst::BigUIntBinaryOperator::Add, biguintConst(1, _loc), _loc), _loc));
	auto operation = awst::makeBlock(_loc);
	operation->body.push_back(awst::makeAssignmentStatement(index, biguintConst(0, _loc), _loc));
	operation->body.push_back(awst::makeWhileLoop(awst::makeNumericCompare(index, awst::NumericComparison::Lt,
		awst::makeBiguintConstant(_count.str(), _loc), _loc), std::move(body), _loc));
	// Tuple assignment reverses component post-effects. Keep initialization
	// and traversal together so that reversal cannot move the loop before it.
	_out.push_back(std::move(operation));
	return true;
}

std::vector<SlotHandleAccess::FieldPos> SlotHandleAccess::fieldPositions(
	solidity::frontend::StructType const* _structType,
	awst::ARC4Struct const* _structWType)
{
	std::vector<FieldPos> out;
	for (auto const& [name, type]: _structWType->fields())
	{
		FieldPos f;
		f.name = name;
		auto const& off = _structType->storageOffsetsOfMember(f.name);
		f.slot = checkedSize<unsigned>(off.first, "struct member slot offset");
		f.byteOffset = off.second;
		f.solType = _structType->memberType(name);
		if (!f.solType) throw std::logic_error("Physical struct field has no solc member");
		f.size = f.solType->storageBytes();
		f.wtype = type;
		out.push_back(std::move(f));
	}
	return out;
}



} // namespace puyasol::builder
