#pragma once

#include "builder/sol-eb/ContractContext.h"
#include "builder/sol-eb/NodeBuilder.h"

#include <libsolidity/ast/AST.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace puyasol::builder::eb
{

/// Result of `rebuildArc4StructChainCOW`.
///   - assignTarget: outermost write target; may be Box/Index/Field where the walk stopped.
///   - assignValue: outermost rebuilt NewStruct with the inner change propagated.
///   - fieldChain: (name, type) pairs walked outward; reverse + FieldExpression on the
///     emitted assignment to recover the originally-targeted field.
struct ArcStructCowResult
{
	std::shared_ptr<awst::Expression> assignTarget;
	std::shared_ptr<awst::Expression> assignValue;
	std::vector<std::pair<std::string, awst::WType const*>> fieldChain;
};

/// Utilities for compound assignment and ARC4 struct COW chain rebuild.
class AssignmentHelper
{
public:
	/// Compute `currentValue {op} rhs`. Returns nullptr if no builder handles the type.
	static std::shared_ptr<awst::Expression> tryComputeCompoundValue(
		ContractContext& _ctx,
		solidity::frontend::Token _assignOp,
		solidity::frontend::Type const* _targetSolType,
		std::shared_ptr<awst::Expression> _currentValue,
		std::shared_ptr<awst::Expression> _rhs,
		awst::SourceLocation const& _loc);

	/// Walk the outer FieldExpression chain, rebuilding a NewStruct at each ARC4Struct level
	/// (copy-on-write). Stops when the base is not an ARC4Struct. StateGet wrappers are
	/// stripped for the write target and preserved for read bases.
	static ArcStructCowResult rebuildArc4StructChainCOW(
		ContractContext& _ctx,
		std::shared_ptr<awst::Expression> _initialTarget,
		std::shared_ptr<awst::Expression> _initialValue,
		awst::SourceLocation const& _loc);

	/// A struct-field COW STORE ready for assembly: writable target + rebuilt
	/// outer value + the walked field chain (reverse it to re-extract the
	/// stored field for an assignment-expression result).
	struct StructFieldCowStore
	{
		std::shared_ptr<awst::Expression> target;
		std::shared_ptr<awst::Expression> value;
		std::vector<std::pair<std::string, awst::WType const*>> fieldChain;
	};

	/// THE struct-field copy-on-write store — shared by SolAssignment
	/// (`s.f = v`, `s.f op= v`, tuple destructure, `s.b[i] = v`) and
	/// SolUnaryOperation (`s.f++`, `--s.f`). Encodes `_fieldValue` at the
	/// field's ARC4 type (no-op if already encoded), reads sibling fields
	/// with-default (fresh box yields defaults instead of reverting),
	/// rebuilds the outer struct chain COW, strips StateGet/ARC4Decode from
	/// the write target, and queues the lazy-root-box ensure (mapping-entry
	/// boxes must exist before box_replace — the inc/dec path historically
	/// skipped this and `n[k][i].f++` on a fresh key died on "no such box").
	/// Callers assemble the statement/expression themselves.
	static StructFieldCowStore buildStructFieldCowStore(
		ContractContext& _ctx,
		awst::FieldExpression const* _fieldExpr,
		awst::ARC4Struct const* _structType,
		std::shared_ptr<awst::Expression> _fieldValue,
		awst::SourceLocation const& _loc);
};

} // namespace puyasol::builder::eb
