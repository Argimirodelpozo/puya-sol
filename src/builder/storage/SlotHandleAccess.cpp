/// @file SlotHandleAccess.cpp
/// See SlotHandleAccess.h.

#include "builder/storage/SlotHandleAccess.h"
#include "builder/storage/SlotWordCodec.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-types/SolIntType.h"
#include "awst/NameGen.h"
#include "Logger.h"

#include <libsolidity/ast/AST.h>

#include <algorithm>
#include <functional>

namespace puyasol::builder
{

namespace
{
std::shared_ptr<awst::Expression> biguintConst(uint64_t v, awst::SourceLocation const& loc)
{
	return awst::makeIntegerConstant(std::to_string(v), loc, awst::WType::biguintType());
}
std::shared_ptr<awst::Expression> u64Const(uint64_t v, awst::SourceLocation const& loc)
{
	return awst::makeIntegerConstant(v, loc);
}
/// small biguint (guaranteed < 2^64 by construction) → uint64
std::shared_ptr<awst::Expression> smallBiguintToU64(
	std::shared_ptr<awst::Expression> e, awst::SourceLocation const& loc)
{
	return awst::makeBtoi(awst::makeExtractLastN(
		awst::makeZeroExtendToN(awst::makeAsBytes(std::move(e), loc), 8, loc), 8, loc), loc);
}
/// bind an expression to a fresh local; returns a reader lambda
template <typename Out>
std::function<std::shared_ptr<awst::Expression>()> bindTemp(
	Out& out, std::shared_ptr<awst::Expression> e, awst::WType const* wt,
	char const* tag, awst::SourceLocation const& loc)
{
	std::string name = std::string("__sha_") + tag + "_"
		+ std::to_string(awst::NameGen::next("SlotHandleAccess.tmp"));
	out.push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression(name, wt, loc), std::move(e), loc));
	return [name, wt, loc]() { return awst::makeVarExpression(name, wt, loc); };
}
} // namespace

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
		l.strideSlots = slots > 4096 ? 4096u : static_cast<unsigned>(slots);
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
			awst::BigUIntBinaryOperator::Mult, biguintConst(_l.strideSlots, _loc), _loc);
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

namespace
{
/// Big-endian byte position of packed element (idx % perSlot) within its word:
/// (32 - size) - (idx % perSlot) * size, as uint64.
std::shared_ptr<awst::Expression> packedBEPos(
	std::shared_ptr<awst::Expression> _idx,
	SlotHandleAccess::ElemLayout const& _l,
	awst::SourceLocation const& _loc)
{
	auto within = awst::makeBigUIntBinOp(std::move(_idx),
		awst::BigUIntBinaryOperator::Mod, biguintConst(_l.perSlot, _loc), _loc);
	auto withinU64 = smallBiguintToU64(std::move(within), _loc);
	auto scaled = awst::makeUInt64BinOp(std::move(withinU64),
		awst::UInt64BinaryOperator::Mult, u64Const(_l.size, _loc), _loc);
	return awst::makeUInt64BinOp(u64Const(32 - _l.size, _loc),
		awst::UInt64BinaryOperator::Sub, std::move(scaled), _loc);
}
/// Sign-extend a canonical biguint element to 256-bit TC when the Solidity
/// element type is signed sub-256. (Unlike typed CELLS, slot-handle elements
/// always travel as canonical biguint, so ≤64-bit signed extends here too.)
std::shared_ptr<awst::Expression> canonSignExtend(
	std::shared_ptr<awst::Expression> _v,
	solidity::frontend::Type const* _solElemType,
	awst::SourceLocation const& _loc)
{
	if (auto it = SolIntType::fromSol(_solElemType); it && it->isSigned && it->bits < 256)
		return TypeCoercion::signExtendToUint256(std::move(_v), it->bits, _loc);
	return _v;
}
} // namespace

std::shared_ptr<awst::Expression> SlotHandleAccess::readScalarElem(
	std::shared_ptr<awst::Expression> _base,
	std::shared_ptr<awst::Expression> _idx,
	ElemLayout const& _l,
	solidity::frontend::Type const* _solElemType,
	awst::SourceLocation const& _loc)
{
	if (_l.perSlot == 1)
		return readSlot(elemSlot(std::move(_base), std::move(_idx), _l, _loc), _loc);
	// packed: extract the element's bytes from its word
	auto word = readSlot(elemSlot(std::move(_base), _idx, _l, _loc), _loc);
	auto wordB = awst::makeLeftPadToN(awst::makeAsBytes(std::move(word), _loc), 32, _loc);
	auto raw = awst::makeExtract3(std::move(wordB), packedBEPos(_idx, _l, _loc),
		u64Const(_l.size, _loc), _loc);
	return canonSignExtend(awst::makeAsBiguint(std::move(raw), _loc), _solElemType, _loc);
}

void SlotHandleAccess::writeScalarElem(
	std::vector<std::shared_ptr<awst::Statement>>& _out,
	std::shared_ptr<awst::Expression> _base,
	std::shared_ptr<awst::Expression> _idx,
	ElemLayout const& _l,
	std::shared_ptr<awst::Expression> _valueBiguint,
	awst::SourceLocation const& _loc)
{
	if (_l.perSlot == 1)
	{
		_out.push_back(writeSlot(
			elemSlot(std::move(_base), std::move(_idx), _l, _loc),
			std::move(_valueBiguint), _loc));
		return;
	}
	// Bind idx + slot once — used in slot math, position math, read AND write.
	auto idxVar = bindTemp(_out, std::move(_idx), awst::WType::biguintType(), "idx", _loc);
	auto slotVar = bindTemp(_out, elemSlot(std::move(_base), idxVar(), _l, _loc),
		awst::WType::biguintType(), "slot", _loc);
	// canonical biguint value → its `size` trailing bytes (the packed TC)
	auto fieldB = awst::makeExtract(
		awst::makeZeroExtendToN(awst::makeAsBytes(std::move(_valueBiguint), _loc), 32, _loc),
		static_cast<int>(32 - _l.size), static_cast<int>(_l.size), _loc);
	auto wordB = awst::makeLeftPadToN(
		awst::makeAsBytes(readSlot(slotVar(), _loc), _loc), 32, _loc);
	auto newWord = awst::makeReplace3(std::move(wordB),
		packedBEPos(idxVar(), _l, _loc), std::move(fieldB), _loc);
	_out.push_back(writeSlot(slotVar(), awst::makeAsBiguint(std::move(newWord), _loc), _loc));
}

namespace
{
struct FieldPos
{
	std::string name;
	unsigned slot = 0;        ///< slot offset within the element
	unsigned byteOffset = 0;  ///< low-order byte offset within that slot
	unsigned size = 0;
	awst::WType const* wtype = nullptr;
	solidity::frontend::Type const* solType = nullptr;
};

std::vector<FieldPos> structFieldPositions(
	solidity::frontend::StructType const* _structType,
	awst::ARC4Struct const* _structWType)
{
	std::vector<FieldPos> out;
	for (auto const& member: _structType->structDefinition().members())
	{
		if (!member)
			continue;
		FieldPos f;
		f.name = member->name();
		auto const& off = _structType->storageOffsetsOfMember(f.name);
		f.slot = static_cast<unsigned>(off.first);
		f.byteOffset = off.second;
		f.solType = member->type();
		f.size = f.solType ? f.solType->storageBytes() : 32;
		for (auto const& [fname, ftype]: _structWType->fields())
			if (fname == f.name) { f.wtype = ftype; break; }
		out.push_back(std::move(f));
	}
	return out;
}
} // namespace

void SlotHandleAccess::writeStructElem(
	std::vector<std::shared_ptr<awst::Statement>>& _out,
	std::shared_ptr<awst::Expression> _elemBaseSlot,
	solidity::frontend::StructType const* _structType,
	awst::ARC4Struct const* _structWType,
	std::shared_ptr<awst::Expression> _structVal,
	awst::SourceLocation const& _loc)
{
	auto fields = structFieldPositions(_structType, _structWType);
	unsigned strideSlots = static_cast<unsigned>(_structType->storageSize());

	// Bind value + base once (fields read the value per slot; base used per slot).
	auto valVar = bindTemp(_out, std::move(_structVal),
		static_cast<awst::WType const*>(_structWType), "sv", _loc);
	auto baseVar = bindTemp(_out, std::move(_elemBaseSlot),
		awst::WType::biguintType(), "sbase", _loc);

	for (unsigned s = 0; s < strideSlots; ++s)
	{
		// Assemble this slot's 32-byte word from its fields (BE left→right,
		// gaps zero) — whole-slot write, matching EVM struct assignment.
		std::vector<FieldPos const*> inSlot;
		for (auto const& f: fields)
			if (f.slot == s && f.wtype && f.size > 0 && f.size <= 32)
				inSlot.push_back(&f);
		std::sort(inSlot.begin(), inSlot.end(), [](auto const* a, auto const* b) {
			return a->byteOffset > b->byteOffset;
		});
		std::shared_ptr<awst::Expression> word;
		auto append = [&](std::shared_ptr<awst::Expression> piece) {
			word = word ? awst::makeConcat(std::move(word), std::move(piece), _loc)
						: std::move(piece);
		};
		unsigned cursor = 0;
		for (auto const* f: inSlot)
		{
			unsigned start = 32 - f->byteOffset - f->size;
			if (start > cursor)
				append(awst::makeBytesConstant(std::vector<uint8_t>(start - cursor, 0), _loc));
			auto fieldVal = awst::makeFieldExpression(valVar(), f->name, f->wtype, _loc);
			append(SlotWordCodec::nativeToPackedBytes(std::move(fieldVal), f->wtype, f->size, _loc));
			cursor = start + f->size;
		}
		if (cursor < 32)
			append(awst::makeBytesConstant(std::vector<uint8_t>(32 - cursor, 0), _loc));

		auto slotExpr = awst::makeBigUIntBinOp(baseVar(),
			awst::BigUIntBinaryOperator::Add, biguintConst(s, _loc), _loc);
		_out.push_back(writeSlot(std::move(slotExpr),
			awst::makeAsBiguint(std::move(word), _loc), _loc));
	}
}

std::shared_ptr<awst::Expression> SlotHandleAccess::readStructElem(
	std::vector<std::shared_ptr<awst::Statement>>& _preOut,
	std::shared_ptr<awst::Expression> _elemBaseSlot,
	solidity::frontend::StructType const* _structType,
	awst::ARC4Struct const* _structWType,
	awst::SourceLocation const& _loc)
{
	auto fields = structFieldPositions(_structType, _structWType);
	unsigned strideSlots = static_cast<unsigned>(_structType->storageSize());

	auto baseVar = bindTemp(_preOut, std::move(_elemBaseSlot),
		awst::WType::biguintType(), "rbase", _loc);

	// One bound word temp per element slot; fields extract at const positions.
	std::vector<std::function<std::shared_ptr<awst::Expression>()>> words;
	for (unsigned s = 0; s < strideSlots; ++s)
	{
		auto slotExpr = awst::makeBigUIntBinOp(baseVar(),
			awst::BigUIntBinaryOperator::Add, biguintConst(s, _loc), _loc);
		auto wordB = awst::makeLeftPadToN(
			awst::makeAsBytes(readSlot(std::move(slotExpr), _loc), _loc), 32, _loc);
		words.push_back(bindTemp(_preOut, std::move(wordB),
			awst::WType::bytesType(), "rword", _loc));
	}

	auto ns = awst::makeNewStruct(_structWType, _loc);
	for (auto const& f: fields)
	{
		if (!f.wtype || f.slot >= words.size())
			continue;
		unsigned start = 32 - f.byteOffset - f.size;
		auto raw = awst::makeExtract(words[f.slot](),
			static_cast<int>(start), static_cast<int>(f.size), _loc);
		auto native = SlotWordCodec::packedBytesToNative(
			std::move(raw), f.wtype, f.solType, f.size, _loc);
		if (native)
			ns->values[f.name] = std::move(native);
	}
	return ns;
}

} // namespace puyasol::builder
