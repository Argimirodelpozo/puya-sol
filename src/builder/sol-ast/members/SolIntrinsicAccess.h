#pragma once

#include "builder/sol-ast/SolMemberAccess.h"

namespace puyasol::builder::sol_ast
{

/// msg.sender, block.timestamp, block.difficulty, block.prevrandao, etc.
/// Exact intrinsics use IntrinsicMapper; EVM-only environment values are
/// classified and lowered through EvmFeaturePolicy.
class SolIntrinsicAccess: public SolMemberAccess
{
public:
	using SolMemberAccess::SolMemberAccess;
	std::shared_ptr<awst::Expression> toAwst() override;
};

} // namespace puyasol::builder::sol_ast
