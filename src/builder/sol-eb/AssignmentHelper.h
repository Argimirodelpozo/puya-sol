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
///   - assignTarget: outermost write target (the root of the access chain
///     where the synthesized NewStruct gets stored). May be a
///     BoxValueExpression / IndexExpression / FieldExpression whose own
///     base wasn't an ARC4Struct, so the walk stopped here.
///   - assignValue: outermost rebuilt struct, with the inner change
///     propagated through all enclosing NewStructs.
///   - fieldChain: list of (fieldName, fieldType) walked outward from the
///     innermost write. Reversing this and calling FieldExpression on the
///     emitted AssignmentExpression yields the originally-targeted field
///     for assignment-as-expression semantics.
struct ArcStructCowResult
{
	std::shared_ptr<awst::Expression> assignTarget;
	std::shared_ptr<awst::Expression> assignValue;
	std::vector<std::pair<std::string, awst::WType const*>> fieldChain;
};

/// Handles compound assignment operations (+=, -=, *=, etc.) via the builder pattern.
///
/// For compound assignment `target op= value`:
///   1. Read current target value
///   2. Compute `current_value op value` via the builder's binary_op()
///   3. Return the computed result (caller handles the actual assignment)
///
/// This replaces the direct call to `buildBinaryOp()` in the old AssignmentBuilder,
/// routing through the type-driven builder for the arithmetic operation.
class AssignmentHelper
{
public:
	/// Compute the compound assignment value: `currentValue {op} rhs`.
	/// Returns nullptr if the builder can't handle this type (fall through to old code).
	static std::shared_ptr<awst::Expression> tryComputeCompoundValue(
		ContractContext& _ctx,
		solidity::frontend::Token _assignOp,
		solidity::frontend::Type const* _targetSolType,
		std::shared_ptr<awst::Expression> _currentValue,
		std::shared_ptr<awst::Expression> _rhs,
		awst::SourceLocation const& _loc);

	/// Walk the outer FieldExpression chain from `_initialTarget`, rebuilding
	/// NewStructs at each ARC4Struct level (copy-on-write). The inner-most
	/// new value is `_initialValue`. Stops when the base is no longer a
	/// FieldExpression whose base resolves to an ARC4Struct.
	///
	/// Used for write-through assignments to nested ARC4Struct fields:
	///   `outer.middle.inner.f = v`
	/// decomposes into rebuilding `outer.middle.inner` (with `f` replaced),
	/// then `outer.middle` (with `inner` replaced by the new struct), then
	/// `outer` (with `middle` replaced) — each level a NewStruct that
	/// copies the unchanged fields and substitutes the inner result.
	///
	/// StateGet wrappers around `outerField->base` get unwrapped for the
	/// write target and rewrapped for the read base (so the surviving
	/// fields inherit a read-shape that StateGet can serve).
	static ArcStructCowResult rebuildArc4StructChainCOW(
		ContractContext& _ctx,
		std::shared_ptr<awst::Expression> _initialTarget,
		std::shared_ptr<awst::Expression> _initialValue,
		awst::SourceLocation const& _loc);
};

} // namespace puyasol::builder::eb
