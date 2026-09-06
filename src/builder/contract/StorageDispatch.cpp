#include "builder/contract/ContractBuilder.h"
#include "builder/contract/StorageDispatchSupport.h"
#include "builder/storage/EvmLayoutMode.h"
#include "builder/storage/StorageLayout.h"
#include "builder/storage/StorageMapper.h"
#include "builder/storage/StorageRuntimePlan.h"
#include "builder/storage/SlotWordCodec.h"
#include "builder/storage/SlotHandleAccess.h"

#include <libsolidity/ast/Types.h>
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-types/SolIntType.h"
#include "awst/HelperMethod.h"
#include "awst/NameGen.h"
#include "Logger.h"

#include <algorithm>
#include <set>

namespace puyasol::builder
{
namespace
{
/// One arm of a STRUCT state var: internal slot `k` and the codec-supported
/// fields living there, BE left→right (byteOffset descending).
struct StructSlotGroup
{
	unsigned k = 0;
	std::vector<SlotHandleAccess::FieldPos> fields;
};

/// A layout slot the dispatch methods route, classified ONCE for both
/// __storage_read and __storage_write so the two cannot disagree about which
/// slots (and which struct field groups) get an arm. `vars` keeps the slot's
/// variableIndices order. The emitters still own the per-backend gates: the
/// length-word bridge exists only for dynamic arrays, its store side only
/// for box-backed ones.
struct DispatchSlot
{
	enum class Kind
	{
		Struct,      ///< lone ARC4Struct-typed struct var: per-internal-slot field arms
		Aggregate,   ///< lone non-value var: dynamic length-word bridge or fallback
		FullSlot,    ///< dynamic slot, or a lone full-slot scalar
		Packed,      ///< sub-word vars sharing the word
	};
	Kind kind = Kind::Packed;
	SlotInfo const* slot = nullptr;
	std::vector<SlotVariable const*> vars;
	awst::ARC4Struct const* structW = nullptr;
	std::vector<StructSlotGroup> groups;
};

// Can SlotWordCodec handle this field? Leaf scalars only — array/struct/
// mapping members occupy their own slots (solc storageBytes==32 for them),
// and their slots keep the box-per-slot fallback. byte[N] passes only when
// the arc4 array length EQUALS the field's packed byte size (a true bytesN;
// uint8[2] arrays report storageBytes 32 and are rejected here).
bool codecSupported(SlotHandleAccess::FieldPos const& f)
{
	auto const* w = f.wtype;
	if (!w) return false;
	if (w == awst::WType::uint64Type() || w == awst::WType::boolType()
		|| w == awst::WType::biguintType() || w == awst::WType::accountType()
		|| w == awst::WType::arc4BoolType())
		return true;
	if (w->kind() == awst::WTypeKind::ARC4UIntN || w->kind() == awst::WTypeKind::Bytes)
		return true;
	if (auto const* sa = dynamic_cast<awst::ARC4StaticArray const*>(w))
		if (auto const* el = dynamic_cast<awst::ARC4UIntN const*>(sa->elementType()))
			return el->n() == 8
				&& sa->arraySize() == static_cast<int64_t>(f.size);   // bytesN-as-byte[N]
	return false;
}

// A struct var's dispatchable internal slots: one group per slot whose
// field set the codec fully supports; the others keep the fallback.
std::vector<StructSlotGroup> structSlotGroups(
	solidity::frontend::StructType const* structType,
	awst::ARC4Struct const* structW)
{
	auto fields = SlotHandleAccess::fieldPositions(structType, structW);
	unsigned stride = static_cast<unsigned>(structType->storageSize() > 64
		? 64 : static_cast<unsigned>(structType->storageSize()));
	std::vector<StructSlotGroup> groups;
	for (unsigned k = 0; k < stride; ++k)
	{
		StructSlotGroup group;
		group.k = k;
		bool ok = true;
		for (auto const& f: fields)
			if (f.slot == k)
			{
				if (!codecSupported(f) || f.size == 0 || f.size > 32)
					ok = false;
				group.fields.push_back(f);
			}
		if (!ok || group.fields.empty())
			continue;
		std::sort(group.fields.begin(), group.fields.end(), [](auto const& a, auto const& b) {
			return a.byteOffset > b.byteOffset;
		});
		groups.push_back(std::move(group));
	}
	return groups;
}

// The slot table: every slot with a typed variable, in layout order.
std::vector<DispatchSlot> collectDispatchSlots(StorageLayout const& layout)
{
	std::vector<DispatchSlot> table;
	for (auto const& si: layout.slots())
	{
		DispatchSlot ds;
		ds.slot = &si;
		for (auto const index: si.variableIndices)
		{
			auto const* v = &layout.variables().at(index);
			if (v && v->wtype && v->wtype != awst::WType::voidType())
				ds.vars.push_back(v);
		}
		if (ds.vars.empty()) continue;

		auto const* v0 = ds.vars[0];
		if (ds.vars.size() == 1)
			if (auto const* structType = dynamic_cast<
					solidity::frontend::StructType const*>(v0->solType))
				if (auto const* structW = dynamic_cast<awst::ARC4Struct const*>(v0->wtype))
				{
					ds.kind = DispatchSlot::Kind::Struct;
					ds.structW = structW;
					ds.groups = structSlotGroups(structType, structW);
					table.push_back(std::move(ds));
					continue;
				}
		if (ds.vars.size() == 1 && v0->solType && !v0->solType->isValueType())
			ds.kind = DispatchSlot::Kind::Aggregate;
		else if (si.isDynamic || (ds.vars.size() == 1 && v0->isFullSlot))
			ds.kind = DispatchSlot::Kind::FullSlot;
		else
			ds.kind = DispatchSlot::Kind::Packed;
		table.push_back(std::move(ds));
	}
	return table;
}

/// The named-cell model's __storage_read/__storage_write emitters plus the
/// cell/codec factories they share. These were `[&]` lambdas inside one
/// 700-line function; the bodies are unchanged — `loc`, `cref` and the
/// mapper resolve as members rather than captures.
struct NamedCellDispatch
{
	StorageMapper& m_storageMapper;
	awst::SourceLocation loc;
	std::string cref;

	auto makeUint64(std::string const& val) const { return awst::makeIntegerConstant(val, loc); }
	auto makeBytes(std::string const& s) const { return awst::makeUtf8BytesConstant(s, loc); }
	auto slotVar() const { return awst::makeVarExpression("__slot", awst::WType::biguintType(), loc); }
	auto valueVar() const { return awst::makeVarExpression("__value", awst::WType::biguintType(), loc); }
	std::string storageName(SlotVariable const* variable) const
	{
		if (!variable)
			return std::string{};
		return variable->declaration
			? m_storageMapper.physicalBindingFor(*variable->declaration).key
			: variable->name;
	}
	bool usesBoxStorage(SlotVariable const* variable) const
	{
		return variable && variable->declaration
			&& m_storageMapper.physicalBindingFor(*variable->declaration).kind
				== awst::AppStorageKind::Box;
	}
	std::shared_ptr<awst::Expression> stateCellRead(SlotVariable const* v)
	{
		if (!v || !v->declaration) return nullptr;
		auto binding = m_storageMapper.physicalBindingFor(*v->declaration);
		return m_storageMapper.createStateRead(binding, loc);
	}
	std::shared_ptr<awst::Expression> stateCellWrite(SlotVariable const* v,
		std::shared_ptr<awst::Expression> value)
	{
		if (!v || !v->declaration) return nullptr;
		auto binding = m_storageMapper.physicalBindingFor(*v->declaration);
		return m_storageMapper.createStateWrite(binding, std::move(value), loc);
	}

	// EVM slot arithmetic wraps mod 2^256 (boundary fixtures repoint an array to
	// 2^256-5 so base+idx crosses zero and lands on named vars). biguint add does
	// NOT wrap, so reduce the incoming slot up front — the old uint64 truncation
	// used to provide this wrap by accident.
	auto makeSlotWrapStmt() const
	{
		auto wrapped = awst::makeBigUIntBinOp(
			slotVar(),
			awst::BigUIntBinaryOperator::Mod,
			awst::makeBiguintConstant(
				"115792089237316195423570985008687907853269984665640564039457584007913129639936",
				loc),
			loc);
		return awst::makeAssignmentStatement(slotVar(), std::move(wrapped), loc);
	}

	// Method skeleton shared by both dispatchers: (__slot[, __value]) biguints.
	awst::ContractMethod makeMethod(
		std::string name, awst::WType const* returnType, bool withValue) const
	{
		// FULL 256-bit slot: EVM slots are 2^256-wide (boundary fixtures probe
		// sub(0,5) = 2^256-5; keccak-derived slots are arbitrary). The old uint64
		// arg silently truncated them at every call site.
		awst::HelperArgs args{{"__slot", awst::WType::biguintType()}};
		if (withValue)
			args.push_back({"__value", awst::WType::biguintType()});
		return awst::makeHelperMethod(cref, std::move(name), returnType, args, loc);
	}

	// Default fallback: BOX-PER-SLOT keyed by the full 32-byte slot ("s:" ++ slot).
	// Replaces the mod-256 __dyn_storage fold (distinct slots aliased) —
	// arbitrary 256-bit slots now get their own 32-byte cell, matching EVM
	// storage semantics (zero-initialised, no collisions). box_create is a
	// no-op when the box already exists at the same (always 32) size.
	auto slotBoxKey() const
	{
		auto slotBytes = awst::makeLeftPadToN(
			awst::makeAsBytes(slotVar(), loc), 32, loc);
		return awst::makeConcat(makeBytes("s:"), std::move(slotBytes), loc);
	}

	// Prepend `if (__slot == slot) arm else <chain so far>`: arms chain
	// bottom-up, the default block innermost.
	void chainArm(
		std::shared_ptr<awst::Block>& elseBlock,
		solidity::u256 const& slot,
		std::shared_ptr<awst::Block> arm) const
	{
		auto cmp = awst::makeNumericCompare(slotVar(), awst::NumericComparison::Eq,
			awst::makeIntegerConstant(slot.str(), loc, awst::WType::biguintType()), loc);
		auto ifElse = awst::makeIfElse(
			std::move(cmp), std::move(arm), std::move(elseBlock), loc);
		auto newElse = awst::makeBlock(loc);
		newElse->body.push_back(std::move(ifElse));
		elseBlock = std::move(newElse);
	}

	// ── Packed-slot codec ────────────────────────────────────────────────────
	// EVM packs multiple sub-word vars into one 32-byte slot; our model stores
	// each var in its OWN typed cell (uint64 global / canonical-TC biguint /
	// bool / bytes[N] / account). sload must ASSEMBLE the EVM word from those
	// cells, sstore must SPLIT the word back — through each var's native repr
	// with exact inverse transforms (mirrors TransientStorage's blob codec).
	// A var at low-order byteOffset o, size s occupies big-endian word bytes
	// [32-o-s, 32-o).

	// One BE piece of a word: `bytes` occupies [start, start+size).
	struct WordPiece
	{
		unsigned start = 0;
		unsigned size = 0;
		std::shared_ptr<awst::Expression> bytes;
	};

	// Concatenate BE-ordered pieces into the full 32-byte word (gaps zero-filled).
	std::shared_ptr<awst::Expression> assembleWord(std::vector<WordPiece> pieces) const
	{
		std::shared_ptr<awst::Expression> word;
		auto append = [&](std::shared_ptr<awst::Expression> piece) {
			word = word ? awst::makeConcat(std::move(word), std::move(piece), loc) : std::move(piece);
		};
		unsigned cursor = 0;
		for (auto& p: pieces)
		{
			if (p.start > cursor)
				append(awst::makeBytesConstant(std::vector<uint8_t>(p.start - cursor, 0), loc));
			append(std::move(p.bytes));
			cursor = p.start + p.size;
		}
		if (cursor < 32)
			append(awst::makeBytesConstant(std::vector<uint8_t>(32 - cursor, 0), loc));
		return word;
	}

	// The var's packed field: s big-endian bytes of its EVM-slot content
	// (typed cell read → SlotWordCodec).
	std::shared_ptr<awst::Expression> packedFieldBytes(SlotVariable const* v)
	{
		auto read = stateCellRead(v);
		return SlotWordCodec::nativeToPackedBytes(std::move(read), v->wtype, v->byteSize, loc);
	}

	// Assemble the full 32-byte word for a packed slot.
	std::shared_ptr<awst::Expression> packedWordBytes(std::vector<SlotVariable const*> vars)
	{
		std::sort(vars.begin(), vars.end(), [](auto const* a, auto const* b) {
			return a->byteOffset > b->byteOffset;   // BE left→right
		});
		std::vector<WordPiece> pieces;
		for (auto const* v: vars)
			pieces.push_back({32 - v->byteOffset - v->byteSize, v->byteSize, packedFieldBytes(v)});
		return assembleWord(std::move(pieces));
	}

	// Split a stored word into per-var writes (appended to _blk).
	void emitPackedStore(std::vector<SlotVariable const*> const& vars, awst::Block& _blk)
	{
		// Bind the padded 32-byte word once — every field extracts from it.
		std::string tmp = "__pk_word_" + std::to_string(awst::NameGen::next("StorageDispatch.pkWord"));
		auto padded = awst::makeLeftPadToN(awst::makeAsBytes(valueVar(), loc), 32, loc);
		_blk.body.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(tmp, awst::WType::bytesType(), loc), std::move(padded), loc));
		auto wordVar = [&]() { return awst::makeVarExpression(tmp, awst::WType::bytesType(), loc); };

		for (auto const* v: vars)
		{
			unsigned sz = v->byteSize;
			unsigned start = 32 - v->byteOffset - sz;
			auto raw = awst::makeExtract(wordVar(), static_cast<int>(start), static_cast<int>(sz), loc);
			auto native = SlotWordCodec::packedBytesToNative(std::move(raw), v->wtype, v->solType, sz, loc);
			if (!native)
				continue;   // codec errored loudly

			auto assign = stateCellWrite(v, std::move(native));
			_blk.body.push_back(awst::makeExpressionStatement(std::move(assign), loc));
		}
	}

	// STRUCT state var: its slots hold packed FIELDS, and the cell is a typed
	// ARC4Struct (box or app-global) — assemble one internal slot's word from
	// the fields living there.
	std::shared_ptr<awst::Expression> structGroupWord(
		SlotVariable const* v, StructSlotGroup const& group)
	{
		std::vector<WordPiece> pieces;
		for (auto const& f: group.fields)
		{
			auto fv = awst::makeFieldExpression(stateCellRead(v), f.name, f.wtype, loc);
			pieces.push_back({32 - f.byteOffset - f.size, f.size,
				SlotWordCodec::nativeToPackedBytes(std::move(fv), f.wtype, f.size, loc)});
		}
		return assembleWord(std::move(pieces));
	}

	// The write side of structGroupWord: split the stored word into the
	// slot's fields via COW on the typed cell.
	void emitStructGroupStore(
		SlotVariable const* v,
		awst::ARC4Struct const* structW,
		StructSlotGroup const& group,
		awst::Block& blk)
	{
		// bind the padded word once
		std::string tmp = "__pk_sw_" + std::to_string(
			awst::NameGen::next("StorageDispatch.pkStructWord"));
		blk.body.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(tmp, awst::WType::bytesType(), loc),
			awst::makeLeftPadToN(awst::makeAsBytes(valueVar(), loc), 32, loc),
			loc));
		auto wordVar = [&]() {
			return awst::makeVarExpression(tmp, awst::WType::bytesType(), loc);
		};
		// COW: rebuild the struct with this slot's fields replaced
		std::set<std::string> replaced;
		for (auto const& f: group.fields)
			replaced.insert(f.name);
		auto ns = awst::makeNewStruct(structW, loc);
		for (auto const& [fname, ftype]: structW->fields())
		{
			if (replaced.count(fname))
			{
				SlotHandleAccess::FieldPos const* fp = nullptr;
				for (auto const& g: group.fields)
					if (g.name == fname) { fp = &g; break; }
				unsigned start = 32 - fp->byteOffset - fp->size;
				auto raw = awst::makeExtract(wordVar(),
					static_cast<int>(start), static_cast<int>(fp->size), loc);
				auto native = SlotWordCodec::packedBytesToNative(
					std::move(raw), fp->wtype, fp->solType, fp->size, loc);
				if (native)
					ns->values[fname] = std::move(native);
			}
			else
				ns->values[fname] = awst::makeFieldExpression(
					stateCellRead(v), fname, ftype, loc);
		}
		blk.body.push_back(awst::makeExpressionStatement(
			awst::makeAssignmentExpression(
				structCellTarget(v), std::move(ns), loc, v->wtype), loc));
	}

	// EVM exposes a dynamic array's length in its root slot. Named AVM cells
	// keep the value itself (ARC4 header/body or raw bytes), so bridge that
	// representation explicitly for assembly sload/sstore(root).
	std::shared_ptr<awst::Expression> dynamicLengthWord(SlotVariable const* v)
	{
		auto const* at = v ? dynamic_cast<solidity::frontend::ArrayType const*>(v->solType)
			: nullptr;
		if (!at || !at->isDynamicallySized()) return nullptr;
		auto cell = stateCellRead(v);
		std::shared_ptr<awst::Expression> len;
		if (at->isByteArrayOrString())
			len = awst::makeLen(std::move(cell), loc);
		else
			len = awst::makeArrayLength(
				std::move(cell), awst::WType::uint64Type(), loc);
		return awst::makeAsBiguint(awst::makeItob(std::move(len), loc), loc);
	}

	bool emitDynamicLengthStore(SlotVariable const* v, awst::Block& blk)
	{
		auto const* at = v ? dynamic_cast<solidity::frontend::ArrayType const*>(v->solType)
			: nullptr;
		if (!at || !at->isDynamicallySized() || !usesBoxStorage(v))
			return false;
		std::string n = "__dyn_len_"
			+ std::to_string(awst::NameGen::next("StorageDispatch.dynLen"));
		auto rawLen = awst::makeExtractLastN(awst::makeLeftPadToN(
			awst::makeAsBytes(valueVar(), loc), 8, loc), 8, loc);
		blk.body.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(n, awst::WType::uint64Type(), loc),
			awst::makeBtoi(std::move(rawLen), loc), loc));
		auto lenVar = [&] {
			return awst::makeVarExpression(n, awst::WType::uint64Type(), loc);
		};
		auto key = [&] { return makeBytes(storageName(v)); };

		std::shared_ptr<awst::Expression> newSize;
		bool hasHeader = !at->isByteArrayOrString();
		if (!hasHeader)
			newSize = lenVar();
		else
		{
			auto const* da = dynamic_cast<awst::ARC4DynamicArray const*>(v->wtype);
			int elemSize = da
				? computeEncodedElementSize(da->elementType()).fixedBytes<int>().value_or(0) : 0;
			if (elemSize <= 0)
			{
				if (!da)
				{
					Logger::instance().error(
						"cannot resize the declared dynamic-array storage type", loc);
					return true;
				}
				// Dynamically encoded children cannot be resized by byte width.
				// Mutate the declared ARC4 array recursively instead: every push uses
				// the child's type-directed default, so T[], T[][], structs, and mixed
				// ranks follow the same path.
				std::string currentName = "__dyn_cur_"
					+ std::to_string(awst::NameGen::next("StorageDispatch.dynCurrent"));
				auto currentVar = [&] {
					return awst::makeVarExpression(
						currentName, awst::WType::uint64Type(), loc);
				};
				auto target = [&] {
					return StorageMapper::makeTopLevelBoxExpr(
						storageName(v), v->wtype, loc);
				};
				blk.body.push_back(awst::makeAssignmentStatement(
					currentVar(), awst::makeArrayLength(
						stateCellRead(v), awst::WType::uint64Type(), loc), loc));

				auto grow = awst::makeBlock(loc);
				grow->body.push_back(awst::makeExpressionStatement(
					awst::makeArrayPushOne(
						target(), StorageMapper::makeDefaultValue(
							da->elementType(), loc), v->wtype, loc), loc));
				grow->body.push_back(awst::makeAssignmentStatement(
					currentVar(), awst::makeUInt64BinOp(
						currentVar(), awst::UInt64BinaryOperator::Add,
						awst::makeIntegerConstant(uint64_t{1}, loc), loc), loc));
				blk.body.push_back(awst::makeWhileLoop(
					awst::makeNumericCompare(
						currentVar(), awst::NumericComparison::Lt, lenVar(), loc),
					std::move(grow), loc));

				auto shrink = awst::makeBlock(loc);
				shrink->body.push_back(awst::makeExpressionStatement(
					awst::makeArrayPop(target(), da->elementType(), loc), loc));
				shrink->body.push_back(awst::makeAssignmentStatement(
					currentVar(), awst::makeUInt64BinOp(
						currentVar(), awst::UInt64BinaryOperator::Sub,
						awst::makeIntegerConstant(uint64_t{1}, loc), loc), loc));
				blk.body.push_back(awst::makeWhileLoop(
					awst::makeNumericCompare(
						lenVar(), awst::NumericComparison::Lt, currentVar(), loc),
					std::move(shrink), loc));
				return true;
			}
			newSize = awst::makeUInt64BinOp(
				awst::makeIntegerConstant(uint64_t{2}, loc),
				awst::UInt64BinaryOperator::Add,
				awst::makeUInt64BinOp(lenVar(), awst::UInt64BinaryOperator::Mult,
					awst::makeIntegerConstant(static_cast<uint64_t>(elemSize), loc), loc), loc);
		}
		auto resize = awst::makeIntrinsicCall(
			"box_resize", awst::WType::voidType(), loc);
		resize->stackArgs.push_back(key());
		resize->stackArgs.push_back(std::move(newSize));
		blk.body.push_back(awst::makeExpressionStatement(std::move(resize), loc));
		if (hasHeader)
		{
			auto hdr = awst::makeExtract(
				awst::makeItob(lenVar(), loc), 6, 2, loc);
			auto put = awst::makeIntrinsicCall(
				"box_replace", awst::WType::voidType(), loc);
			put->stackArgs.push_back(key());
			put->stackArgs.push_back(awst::makeIntegerConstant(uint64_t{0}, loc));
			put->stackArgs.push_back(std::move(hdr));
			blk.body.push_back(awst::makeExpressionStatement(std::move(put), loc));
		}
		return true;
	}
	std::shared_ptr<awst::Expression> structCellTarget(SlotVariable const* v) const
	{
		auto name = storageName(v);
		if (usesBoxStorage(v))
			return StorageMapper::makeTopLevelBoxExpr(name, v->wtype, loc);
		auto key = awst::makeUtf8BytesConstant(name, loc, awst::WType::stateKeyType());
		return awst::makeAppStateExpression(std::move(key), v->wtype, loc);
	}

	// ── __storage_read(slot: uint64) -> biguint ──
	awst::ContractMethod emitRead(std::vector<DispatchSlot> const& table)
	{
		auto readSub = makeMethod("__storage_read", awst::WType::biguintType(), false);
		auto body = awst::makeBlock(loc);
		body->body.push_back(makeSlotWrapStmt());

		// Build if/else chain for known slots (bottom-up; default = dynamic fallback).
		auto elseBlock = awst::makeBlock(loc);
		{
			auto key = slotBoxKey();
			auto boxCreate = awst::makeBoxCreate(key, makeUint64("32"), loc);
			elseBlock->body.push_back(
				awst::makeExpressionStatement(std::move(boxCreate), loc));

			auto boxExtract = awst::makeBoxExtract(
				key, makeUint64("0"), makeUint64("32"), loc);
			auto cast = awst::makeAsBiguint(std::move(boxExtract), loc);
			elseBlock->body.push_back(
				awst::makeReturnStatement(std::move(cast), loc));
		}

		for (auto const& ds: table)
		{
			auto const* v = ds.vars[0];
			if (ds.kind == DispatchSlot::Kind::Struct)
			{
				// One compare per internal slot whose field group the codec
				// fully supports; others keep the fallback.
				for (auto const& group: ds.groups)
				{
					auto blkK = awst::makeBlock(loc);
					blkK->body.push_back(awst::makeReturnStatement(
						awst::makeAsBiguint(structGroupWord(v, group), loc), loc));
					chainArm(elseBlock, ds.slot->slotNumber + group.k, std::move(blkK));
				}
				continue;
			}

			// The packed-word codec is a LEAF codec. A whole aggregate is never
			// a scalar just because solc assigns its root a full slot. Dynamic
			// arrays/bytes expose their length word through the dedicated bridge;
			// other aggregate slots retain the sparse raw-slot fallback.
			if (ds.kind == DispatchSlot::Kind::Aggregate)
			{
				auto aggregateWord = dynamicLengthWord(v);
				if (!aggregateWord)
					continue;
				auto aggregateBlock = awst::makeBlock(loc);
				aggregateBlock->body.push_back(
					awst::makeReturnStatement(std::move(aggregateWord), loc));
				chainArm(elseBlock, ds.slot->slotNumber, std::move(aggregateBlock));
				continue;
			}

			auto ifBlock = awst::makeBlock(loc);
			if (ds.kind == DispatchSlot::Kind::FullSlot)
			{
				auto cast = dynamicLengthWord(v);
				if (!cast)
				{
					auto read = stateCellRead(v);
					auto raw = SlotWordCodec::nativeToPackedBytes(
						std::move(read), v->wtype, 32, loc);
					cast = awst::makeAsBiguint(std::move(raw), loc);
				}
				ifBlock->body.push_back(awst::makeReturnStatement(std::move(cast), loc));
			}
			else
			{
				// Packed slot: assemble the EVM word from each var's typed cell.
				auto word = awst::makeAsBiguint(packedWordBytes(ds.vars), loc);
				ifBlock->body.push_back(awst::makeReturnStatement(std::move(word), loc));
			}
			chainArm(elseBlock, ds.slot->slotNumber, std::move(ifBlock));
		}

		for (auto& stmt: elseBlock->body)
			body->body.push_back(std::move(stmt));

		readSub.body = body;
		return readSub;
	}

	// ── __storage_write(slot: uint64, value: biguint) -> void ──
	awst::ContractMethod emitWrite(std::vector<DispatchSlot> const& table)
	{
		auto writeSub = makeMethod("__storage_write", awst::WType::voidType(), true);
		auto body = awst::makeBlock(loc);
		body->body.push_back(makeSlotWrapStmt());

		auto elseBlock = awst::makeBlock(loc);
		{
			// BOX-PER-SLOT (see __storage_read): key = "s:" ++ 32-byte slot.
			auto key = slotBoxKey();
			auto paddedVal = awst::makeLeftPadToN(
				awst::makeAsBytes(valueVar(), loc), 32, loc);

			auto boxCreate = awst::makeBoxCreate(key, makeUint64("32"), loc);
			elseBlock->body.push_back(
				awst::makeExpressionStatement(std::move(boxCreate), loc));

			auto boxReplace = awst::makeIntrinsicCall("box_replace", awst::WType::voidType(), loc);
			boxReplace->stackArgs.push_back(key);
			boxReplace->stackArgs.push_back(makeUint64("0"));
			boxReplace->stackArgs.push_back(std::move(paddedVal));
			elseBlock->body.push_back(
				awst::makeExpressionStatement(std::move(boxReplace), loc));

			elseBlock->body.push_back(awst::makeReturnStatement(nullptr, loc));
		}

		for (auto const& ds: table)
		{
			auto const* v = ds.vars[0];
			if (ds.kind == DispatchSlot::Kind::Struct)
			{
				// STRUCT state var (see the read side): split the stored word into
				// the slot's fields via COW on the typed cell.
				for (auto const& group: ds.groups)
				{
					auto blkK = awst::makeBlock(loc);
					emitStructGroupStore(v, ds.structW, group, *blkK);
					blkK->body.push_back(awst::makeReturnStatement(nullptr, loc));
					chainArm(elseBlock, ds.slot->slotNumber + group.k, std::move(blkK));
				}
				continue;
			}

			// Mirror the read-side aggregate gate. Only a box-backed dynamic
			// aggregate has a representation-preserving root-word store here;
			// every other non-value shape must use the sparse fallback rather
			// than being reinterpreted as a packed scalar.
			if (ds.kind == DispatchSlot::Kind::Aggregate)
			{
				auto aggregateBlock = awst::makeBlock(loc);
				if (!emitDynamicLengthStore(v, *aggregateBlock))
					continue;
				aggregateBlock->body.push_back(
					awst::makeReturnStatement(nullptr, loc));
				chainArm(elseBlock, ds.slot->slotNumber, std::move(aggregateBlock));
				continue;
			}

			auto ifBlock = awst::makeBlock(loc);
			if (ds.kind == DispatchSlot::Kind::FullSlot)
			{
				if (!emitDynamicLengthStore(v, *ifBlock))
				{
					auto raw = awst::makeExtractLastN(awst::makeLeftPadToN(
						awst::makeAsBytes(valueVar(), loc), 32, loc), 32, loc);
					auto native = SlotWordCodec::packedBytesToNative(
						std::move(raw), v->wtype, v->solType, 32, loc);
					if (native)
						ifBlock->body.push_back(awst::makeExpressionStatement(
							stateCellWrite(v, std::move(native)), loc));
				}
				ifBlock->body.push_back(awst::makeReturnStatement(nullptr, loc));
			}
			else
			{
				// Packed slot: split the word into each var's typed cell.
				emitPackedStore(ds.vars, *ifBlock);
				ifBlock->body.push_back(awst::makeReturnStatement(nullptr, loc));
			}
			chainArm(elseBlock, ds.slot->slotNumber, std::move(ifBlock));
		}

		for (auto& stmt: elseBlock->body)
			body->body.push_back(std::move(stmt));

		writeSub.body = body;
		return writeSub;
	}
};
} // namespace

void ContractBuilder::buildStorageDispatch(
	StorageRuntimePlan const& _storagePlan,
	awst::Contract* _contractNode,
	std::string const& _contractName
)
{
	if (m_typeMapper.profile().evmStorageLayout)
	{
		buildEvmSlotStorageDispatch(_storagePlan, _contractNode, _contractName);
		return;
	}

	auto const& layout = _storagePlan.solidityLayout;
	awst::SourceLocation loc;
	loc.file = m_sourceFile;

	NamedCellDispatch dispatch{m_storageMapper, loc, m_contractId};
	auto const table = collectDispatchSlots(layout);
	_contractNode->methods.push_back(dispatch.emitRead(table));
	_contractNode->methods.push_back(dispatch.emitWrite(table));

	storage_dispatch::promoteMethods(*_contractNode, m_dispatchSubroutines, m_contractId + ".",
		{"__storage_read", "__storage_write"});

	Logger::instance().debug(
		"Generated __storage_read/__storage_write dispatch for "
		+ std::to_string(layout.totalSlots()) + " slots", loc);
}


} // namespace puyasol::builder
