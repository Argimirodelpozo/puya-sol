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
};

} // namespace puyasol::builder::eb
