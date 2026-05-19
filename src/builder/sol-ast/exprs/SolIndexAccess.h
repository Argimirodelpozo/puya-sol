#pragma once

#include "builder/sol-ast/SolExpression.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder::sol_ast
{

/// Array/mapping index access: arr[i], mapping[key].
/// Handles box storage, nested mappings, sha256 key encoding, sol-eb builder dispatch.
class SolIndexAccess: public SolExpression
{
public:
	SolIndexAccess(eb::ContractContext& _ctx, solidity::frontend::IndexAccess const& _node);
	std::shared_ptr<awst::Expression> toAwst() override;

private:
	solidity::frontend::IndexAccess const& m_indexAccess;

	std::shared_ptr<awst::Expression> handleDynamicArrayAccess();
	std::shared_ptr<awst::Expression> handleMappingAccess();
	std::shared_ptr<awst::Expression> handleRegularIndex();
	std::shared_ptr<awst::Expression> handleSlicedIndex();

	/// Multi-box state-var array access: emits page-aware box_extract/box_replace.
	/// `_idxExpr` is the element index (uint64 or biguint, will be coerced).
	/// `_arrWtype` must be an ARC4StaticArray flagged multi-box by StorageMapper.
	std::shared_ptr<awst::Expression> buildMultiBoxAccess(
		std::string const& _varName,
		awst::WType const* _arrWtype,
		std::shared_ptr<awst::Expression> _idxExpr);

	/// Phase-extract of `handleMappingAccess`: builds the initial bytes
	/// prefix that the per-layer sha256 chain starts from. Picks among
	/// four sources in priority order:
	///   1. mapping-storage-ref param  → runtime VarExpression(<paramName>)
	///   2. `f()[k]` call cursor       → result of the call, coerced to bytes
	///   3. alias-override prefix      → key of the aliased state slot
	///   4. plain state var            → `BytesConstant(varName)` (literal)
	std::shared_ptr<awst::Expression> buildInitialPrefix(
		solidity::frontend::Expression const* _cursor,
		std::string const& _varName,
		std::shared_ptr<awst::Expression> _aliasOverridePrefix);

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

	/// Result of `resolveCursorContext`: the metadata needed by
	/// `handleMappingAccess` to derive the initial storage-key prefix
	/// from the cursor expression (after wrapper peeling).
	struct CursorContext
	{
		std::string varName;                       // Identifier/MemberAccess name
		solidity::frontend::Type const* rootMappingType = nullptr;
		std::shared_ptr<awst::Expression> aliasOverridePrefix;  // alias's slot key, when applicable
	};

	/// Phase-extract of `handleMappingAccess`: given the cursor
	/// expression (already peeled of Assignment/TupleExpression
	/// wrappers), classify it as Identifier (possibly aliased),
	/// MemberAccess, or mapping-returning FunctionCall, and return
	/// the per-shape context the prefix builder needs.
	CursorContext resolveCursorContext(
		solidity::frontend::Expression const* _cursor);
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
