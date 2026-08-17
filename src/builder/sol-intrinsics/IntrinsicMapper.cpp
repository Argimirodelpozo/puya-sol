#include "builder/sol-intrinsics/IntrinsicMapper.h"

namespace puyasol::builder
{

std::shared_ptr<awst::IntrinsicCall> IntrinsicMapper::tryMapMemberAccess(
	std::string const& _objectName,
	std::string const& _memberName,
	awst::SourceLocation const& _loc
)
{
	if (_objectName == "msg")
	{
		if (_memberName == "sender")
			return awst::makeTxn("Sender", awst::WType::accountType(), _loc);
	}
	else if (_objectName == "block")
	{
		if (_memberName == "timestamp")
			return awst::makeGlobal("LatestTimestamp", awst::WType::uint64Type(), _loc);
		if (_memberName == "number")
			return awst::makeGlobal("Round", awst::WType::uint64Type(), _loc);
	}

	return nullptr;
}

} // namespace puyasol::builder
