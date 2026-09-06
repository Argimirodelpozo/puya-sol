#pragma once

#include "builder/sol-ast/SolExpression.h"

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/Types.h>

namespace puyasol::builder::sol_ast
{

/// Array/mapping index access: arr[i], mapping[key].
/// Handles box storage, nested mappings, sha256 key encoding, sol-eb builder dispatch.
class SolIndexAccess: public SolExpression
{
public:
	SolIndexAccess(eb::ContractContext& _ctx, solidity::frontend::IndexAccess const& _node);
	std::shared_ptr<awst::Expression> toAwst() override;

	/// Walk an access chain (`a`, `a[i]`, `p.field[i][j]`, `p.f.x`) to its root.
	/// If rooted at a blob-backed memory aggregate, returns the accumulated
	/// uint64 byte-offset (base + Σ index*stride + field offsets); else nullptr.
	/// Shared by SolIndexAccess (read), SolAssignment (write), SolMemberAccess.
	static std::shared_ptr<awst::Expression> resolveBlobOffset(
		eb::ContractContext& _ctx, Context& _scope,
		solidity::frontend::Expression const& _node,
		awst::SourceLocation const& _loc);

	/// Materialise a VALUE read from the blob at `_off` for a leaf of Solidity
	/// type `_solType`: a scalar leaf → `asBiguint(readMemWordDirect)`; a small
	/// (<=SLOT_SIZE) struct/static-array leaf → `reinterpret(readMemRangeDirect,
	/// arc4Type)`. Returns nullptr for aggregates too large to hold as a single
	/// value (caller should fall back / pass a sub-offset). Consumes `_off`.
	static std::shared_ptr<awst::Expression> readBlobValue(
		eb::ContractContext& _ctx, std::shared_ptr<awst::Expression> _off,
		solidity::frontend::Type const* _solType,
		awst::SourceLocation const& _loc);

private:
	solidity::frontend::IndexAccess const& m_indexAccess;

	std::shared_ptr<awst::Expression> handleDynamicArrayAccess();
	std::shared_ptr<awst::Expression> handleMappingAccess();
	std::shared_ptr<awst::Expression> handleRegularIndex();
	std::shared_ptr<awst::Expression> handleSlicedIndex();

	/// Sign-extend a decoded signed sub-256 array element (e.g. `int128`) from
	/// its raw N-bit two's complement to the canonical 256-bit biguint, so that
	/// `a[i]` compares/arithmetics equal to a sign-extended scalar of the same
	/// type. No-op for unsigned, int256 (already canonical), and <=64-bit
	/// (uint64-backed) elements. `_decoded` is the post-ARC4Decode native value;
	/// the Solidity element type is read from `m_indexAccess.annotation().type`.
	std::shared_ptr<awst::Expression> signExtendSignedElement(
		std::shared_ptr<awst::Expression> _decoded);

	/// Multi-box state-var array access: emits page-aware box_extract/box_replace.
	/// `_idxExpr` is the element index (uint64 or biguint, will be coerced).
	/// `_arrWtype` must be an ARC4StaticArray flagged multi-box by StorageMapper.
	std::shared_ptr<awst::Expression> buildMultiBoxAccess(
		std::string const& _varName,
		awst::WType const* _arrWtype,
		std::shared_ptr<awst::Expression> _idxExpr);

	/// Walk the root mapping/array type for `_numLevels` index steps,
	/// returning the declared key wtype at each level (nullptr at array
	/// levels). Used to coerce each runtime index expression to the
	/// canonical encoding type before hashing.
	std::vector<awst::WType const*> resolveKeyWTypes(
		solidity::frontend::Type const* _rootType, size_t _numLevels);

	/// Compute the value wtype reached after applying every mapping
	/// layer in `_baseType`. For non-mapping base types, returns the
	/// type-mapped wtype of the index expression itself.
	awst::WType const* resolveValueWType(solidity::frontend::Type const* _baseType);


};

/// arr[start:end] range access.
class SolIndexRangeAccess: public SolExpression
{
public:
	SolIndexRangeAccess(eb::ContractContext& _ctx, solidity::frontend::IndexRangeAccess const& _node);
	std::shared_ptr<awst::Expression> toAwst() override;

private:
	solidity::frontend::IndexRangeAccess const& m_rangeAccess;
};

} // namespace puyasol::builder::sol_ast
