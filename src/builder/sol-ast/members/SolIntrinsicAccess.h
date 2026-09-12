#pragma once

#include "builder/sol-ast/SolMemberAccess.h"

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
	/// The selected Solidity address namespace, including verified xchain claims.
	/// Native lifecycle gates must use this same identity as source msg.sender.
	static std::shared_ptr<awst::Expression> sender(
		eb::ContractContext& _ctx, awst::SourceLocation const& _loc);
};

} // namespace puyasol::builder::sol_ast
