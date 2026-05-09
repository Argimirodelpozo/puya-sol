#pragma once

#include "awst/Node.h"

#include <memory>
#include <string>

namespace puyasol::builder
{

/// Maps Solidity built-in expressions (msg.sender, block.timestamp, etc.)
/// to AWST IntrinsicCall nodes.
class IntrinsicMapper
{
public:
	/// Try to map a member access expression (e.g., msg.sender).
	/// Returns nullptr if not a recognized intrinsic.
	static std::shared_ptr<awst::IntrinsicCall> tryMapMemberAccess(
		std::string const& _objectName,
		std::string const& _memberName,
		awst::SourceLocation const& _loc
	);
};

} // namespace puyasol::builder
