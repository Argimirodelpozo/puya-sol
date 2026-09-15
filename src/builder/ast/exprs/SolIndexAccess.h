#pragma once

#include "builder/ast/SolExpression.h"

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
	/// A reference-typed leaf (`p.items`, `a[i]` of an array of arrays) is
	/// dereferenced to the child object unless `_derefLeaf` is false, which
	/// yields the pointer slot itself (a reference-slot write).
	static std::shared_ptr<awst::Expression> resolveBlobOffset(
		eb::ContractContext& _ctx, Context& _scope,
		solidity::frontend::Expression const& _node,
		awst::SourceLocation const& _loc,
		bool _derefLeaf = true);

	/// Optional reference together with its scoped address-evaluation effects.
	/// Failure publishes no effects, so a fresh-value fallback evaluates the
	/// source only once. Modifier parameters use this same physical resolver.
	static std::optional<eb::ContractContext::LoweredExpression> resolveBlobReference(
		eb::ContractContext& _ctx, Context& _scope,
		solidity::frontend::Expression const& _node, awst::SourceLocation const& _loc);

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

	/// Decode to the solc element type, including canonical signed carriers.
	std::shared_ptr<awst::Expression> readElement(
		std::shared_ptr<awst::Expression> _value);

	/// Multi-box state-var array access: emits page-aware box_extract/box_replace.
	/// `_idxExpr` is the element index (uint64 or biguint, will be coerced).
	/// `_arrWtype` must be an ARC4StaticArray flagged multi-box by StorageMapper.
	std::shared_ptr<awst::Expression> buildMultiBoxAccess(
		std::string const& _varName,
		awst::WType const* _arrWtype,
		std::shared_ptr<awst::Expression> _idxExpr);

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

	struct Bounds { std::shared_ptr<awst::Expression> start, end; };
	struct Slice { std::shared_ptr<awst::Expression> base, offset, length; };
	/// Flatten non-byte array slices for indexing/length without materializing them.
	static std::optional<Slice> resolveSlice(eb::ContractContext& _ctx,
		solidity::frontend::Expression const& _source, awst::SourceLocation const& _loc);
	/// Evaluate bounds once, reject wide indexes before narrowing, and assert
	/// solc's start <= end <= parent length even when the result is discarded.
	static Bounds resolveBounds(eb::ContractContext& _ctx,
		solidity::frontend::IndexRangeAccess const& _range,
		std::shared_ptr<awst::Expression> _length, awst::SourceLocation const& _loc);

private:
	solidity::frontend::IndexRangeAccess const& m_rangeAccess;
};

} // namespace puyasol::builder::sol_ast
