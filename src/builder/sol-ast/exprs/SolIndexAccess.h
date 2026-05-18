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
