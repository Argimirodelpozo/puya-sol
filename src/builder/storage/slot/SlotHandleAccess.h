#pragma once

/// @file SlotHandleAccess.h
/// Element access through a SLOT HANDLE — a biguint EVM slot number bound from
/// inline assembly (`_x.slot := <expr>` on a storage-pointer local/return).
/// EVM addresses storage in 32-byte words; our values live in native reprs
/// (canonical biguint scalars, ARC-4 structs). These helpers do the element
/// (slot, byteOffset) arithmetic and cross the word boundary exclusively via
/// SlotWordCodec — the word never leaks past the storage edge.

#include "awst/Node.h"

#include "builder/solc/SolcFwd.h"
#include <libsolutil/Numeric.h>
#include <functional>

namespace puyasol::builder
{

class TypeMapper;

struct SlotHandleAccess
{
	/// How elements of a fixed array lay out in EVM slots.
	struct ElemLayout
	{
		solidity::u256 strideSlots = 1; ///< solc logical stride, not a materialized size
		unsigned perSlot = 1;      ///< packed elems: elements per slot (strideSlots==1)
		unsigned size = 32;        ///< element byte size within its slot
	};
	static ElemLayout layoutFor(solidity::frontend::Type const* _elemType);

	/// FIXED-size arrays: queue `assert(idx < length)` into _preStmts (EVM
	/// Panic 0x32 shape — OOB slot-handle access would silently read/write a
	/// NEIGHBORING state variable's slot) and return an eval-once'd idx that
	/// is safe to reference again in the address math. Dynamic arrays and a
	/// null _arrType pass through untouched.
	static std::shared_ptr<awst::Expression> boundsCheckIndex(
		std::vector<std::shared_ptr<awst::Statement>>& _preStmts,
		std::shared_ptr<awst::Expression> _idx,
		solidity::frontend::ArrayType const* _arrType,
		awst::SourceLocation const& _loc);

	/// Slot of element `idx` (both biguint): base + idx*stride (multi-slot) or
	/// base + idx/perSlot (packed).
	static std::shared_ptr<awst::Expression> elemSlot(
		std::shared_ptr<awst::Expression> _base,
		std::shared_ptr<awst::Expression> _idx,
		ElemLayout const& _l,
		awst::SourceLocation const& _loc);

	/// __puyasol___storage_read(slot) → biguint word.
	static std::shared_ptr<awst::Expression> readSlot(
		std::shared_ptr<awst::Expression> _slot, awst::SourceLocation const& _loc);
	/// __puyasol___storage_write(slot, word) statement.
	static std::shared_ptr<awst::Statement> writeSlot(
		std::shared_ptr<awst::Expression> _slot,
		std::shared_ptr<awst::Expression> _valueBiguint,
		awst::SourceLocation const& _loc);

	/// Emit a typed storage operation for each index in an exact solc extent.
	/// Tiny extents are unrolled; larger ones share a counted loop body.
	static bool forEachIndex(solidity::u256 const& _count,
		std::vector<std::shared_ptr<awst::Statement>>& _out,
		awst::SourceLocation const& _loc,
		std::function<bool(std::shared_ptr<awst::Expression>,
			std::vector<std::shared_ptr<awst::Statement>>&)> const& _emit);

	/// Packed/full scalar element read → CANONICAL biguint (signed elems
	/// sign-extended to 256-bit TC).
	static std::shared_ptr<awst::Expression> readScalarElem(
		std::shared_ptr<awst::Expression> _base,
		std::shared_ptr<awst::Expression> _idx,
		ElemLayout const& _l,
		solidity::frontend::Type const* _solElemType,
		awst::SourceLocation const& _loc);

	/// Packed/full scalar element write (value = canonical biguint).
	/// Emits statements into _out (packed = word read-modify-write).
	static void writeScalarElem(
		std::vector<std::shared_ptr<awst::Statement>>& _out,
		std::shared_ptr<awst::Expression> _base,
		std::shared_ptr<awst::Expression> _idx,
		ElemLayout const& _l,
		std::shared_ptr<awst::Expression> _valueBiguint,
		awst::SourceLocation const& _loc);

	/// A struct field's storage position within its element/variable.
	struct FieldPos
	{
		std::string name;
		unsigned slot = 0;        ///< slot offset within the element
		unsigned byteOffset = 0;  ///< low-order byte offset within that slot
		unsigned size = 0;
		awst::WType const* wtype = nullptr;
		solidity::frontend::Type const* solType = nullptr;
	};
	static std::vector<FieldPos> fieldPositions(
		solidity::frontend::StructType const* _structType,
		awst::ARC4Struct const* _structWType);

};

} // namespace puyasol::builder
