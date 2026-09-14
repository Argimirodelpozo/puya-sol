#pragma once

#include "builder/ast/SolMemberAccess.h"

namespace puyasol::builder::sol_ast
{

/// msg.sender, block.timestamp, block.difficulty, block.prevrandao, etc.
/// solc MagicType owns builtin identity. Non-exact environment values are
/// explicitly classified and lowered through EvmFeaturePolicy.
class SolIntrinsicAccess: public SolMemberAccess
{
public:
	using SolMemberAccess::SolMemberAccess;
	std::shared_ptr<awst::Expression> toAwst() override;
};

} // namespace puyasol::builder::sol_ast
