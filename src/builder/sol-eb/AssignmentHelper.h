#pragma once

#include "builder/sol-eb/BuilderContext.h"
#include "builder/sol-eb/NodeBuilder.h"

#include <libsolidity/ast/AST.h>

#include <memory>

namespace puyasol::builder::eb
{

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
		BuilderContext& _ctx,
		solidity::frontend::Token _assignOp,
		solidity::frontend::Type const* _targetSolType,
		std::shared_ptr<awst::Expression> _currentValue,
		std::shared_ptr<awst::Expression> _rhs,
		awst::SourceLocation const& _loc);
};

} // namespace puyasol::builder::eb
