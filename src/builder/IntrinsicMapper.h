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

	/// Create a "log" intrinsic call for event emission.
	static std::shared_ptr<awst::IntrinsicCall> createLog(
		std::vector<std::shared_ptr<awst::Expression>> _args,
		awst::SourceLocation const& _loc
	);

	/// Create an "assert" intrinsic call (for require).
	static std::shared_ptr<awst::AssertExpression> createAssert(
		std::shared_ptr<awst::Expression> _condition,
		std::optional<std::string> _message,
		awst::SourceLocation const& _loc
	);
};

} // namespace puyasol::builder
