#include "builder/IntrinsicMapper.h"
#include "Logger.h"

namespace puyasol::builder
{

std::shared_ptr<awst::IntrinsicCall> IntrinsicMapper::tryMapMemberAccess(
	std::string const& _objectName,
	std::string const& _memberName,
	awst::SourceLocation const& _loc
)
{
	auto call = std::make_shared<awst::IntrinsicCall>();
	call->sourceLocation = _loc;

	if (_objectName == "msg")
	{
		if (_memberName == "sender")
		{
			call->opCode = "txn";
			call->immediates = {std::string("Sender")};
			call->wtype = awst::WType::accountType();
			return call;
		}
		else if (_memberName == "value")
		{
			// msg.value → payment amount (not directly applicable on Algorand,
			// but map to group txn amount)
			Logger::instance().warning("msg.value mapped to group txn amount (approximate)", _loc);
			call->opCode = "gtxn";
			call->immediates = {0, std::string("Amount")};
			call->wtype = awst::WType::uint64Type();
			return call;
		}
		else if (_memberName == "data")
		{
			// msg.data → application args
			call->opCode = "txna";
			call->immediates = {std::string("ApplicationArgs"), 0};
			call->wtype = awst::WType::bytesType();
			return call;
		}
	}
	else if (_objectName == "block")
	{
		if (_memberName == "timestamp")
		{
			call->opCode = "global";
			call->immediates = {std::string("LatestTimestamp")};
			call->wtype = awst::WType::uint64Type();
			return call;
		}
		else if (_memberName == "number")
		{
			call->opCode = "global";
			call->immediates = {std::string("Round")};
			call->wtype = awst::WType::uint64Type();
			return call;
		}
		else if (_memberName == "chainid")
		{
			// No direct equivalent on Algorand; use global CurrentApplicationID as placeholder
			Logger::instance().warning("block.chainid has no Algorand equivalent, using app ID", _loc);
			call->opCode = "global";
			call->immediates = {std::string("CurrentApplicationID")};
			call->wtype = awst::WType::uint64Type();
			return call;
		}
	}
	else if (_objectName == "tx")
	{
		if (_memberName == "origin")
		{
			call->opCode = "txn";
			call->immediates = {std::string("Sender")};
			call->wtype = awst::WType::accountType();
			return call;
		}
	}

	return nullptr;
}

std::shared_ptr<awst::IntrinsicCall> IntrinsicMapper::createLog(
	std::vector<std::shared_ptr<awst::Expression>> _args,
	awst::SourceLocation const& _loc
)
{
	auto call = std::make_shared<awst::IntrinsicCall>();
	call->sourceLocation = _loc;
	call->opCode = "log";
	call->wtype = awst::WType::voidType();
	call->stackArgs = std::move(_args);
	return call;
}

std::shared_ptr<awst::AssertExpression> IntrinsicMapper::createAssert(
	std::shared_ptr<awst::Expression> _condition,
	std::optional<std::string> _message,
	awst::SourceLocation const& _loc
)
{
	auto expr = std::make_shared<awst::AssertExpression>();
	expr->sourceLocation = _loc;
	expr->wtype = awst::WType::voidType();
	expr->condition = std::move(_condition);
	expr->errorMessage = std::move(_message);
	return expr;
}

} // namespace puyasol::builder
