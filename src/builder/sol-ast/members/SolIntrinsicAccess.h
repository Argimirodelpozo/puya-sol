#pragma once

#include "builder/sol-ast/SolMemberAccess.h"

namespace puyasol::builder::sol_ast
{

/// msg.sender, block.timestamp, block.difficulty, block.prevrandao, etc.
/// Delegates to IntrinsicMapper for standard intrinsics; handles block.prevrandao
/// and block.difficulty specially.
class SolIntrinsicAccess: public SolMemberAccess
{
public:
	using SolMemberAccess::SolMemberAccess;
	std::shared_ptr<awst::Expression> toAwst() override;
};

} // namespace puyasol::builder::sol_ast
