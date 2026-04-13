#include "builder/sol-intrinsics/IntrinsicMapper.h"
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
			// msg.value is now handled in SolIntrinsicAccess.cpp with a
			// conditional expression for safe GroupIndex access.
			// This fallback is kept for non-member-access contexts.
			call->opCode = "gtxns";
			call->immediates = {std::string("Amount")};

			auto groupIdx = std::make_shared<awst::IntrinsicCall>();
			groupIdx->sourceLocation = _loc;
			groupIdx->wtype = awst::WType::uint64Type();
			groupIdx->opCode = "txn";
			groupIdx->immediates = {std::string("GroupIndex")};
			auto one = std::make_shared<awst::IntegerConstant>();
			one->sourceLocation = _loc;
			one->wtype = awst::WType::uint64Type();
			one->value = "1";
			auto payIdx = std::make_shared<awst::UInt64BinaryOperation>();
			payIdx->sourceLocation = _loc;
			payIdx->wtype = awst::WType::uint64Type();
			payIdx->left = std::move(groupIdx);
			payIdx->op = awst::UInt64BinaryOperator::Sub;
			payIdx->right = std::move(one);

			call->stackArgs.push_back(std::move(payIdx));
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
		else if (_memberName == "sig")
		{
			// msg.sig → first 4 bytes of msg.data (ARC4 selector).
			// Routed through SolIntrinsicAccess for a bytes[4]-typed result.
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
			// AVM has no chain ID. The closest equivalent is GenesisHash which
			// uniquely identifies the network (mainnet/testnet/localnet).
			Logger::instance().warning(
				"block.chainid mapped to global GenesisHash. "
				"AVM has no chain ID; GenesisHash uniquely identifies the network", _loc);
			call->opCode = "global";
			call->immediates = {std::string("GenesisHash")};
			call->wtype = awst::WType::bytesType();
			return call;
		}
		else if (_memberName == "coinbase")
		{
			// block.coinbase is the miner address on EVM. AVM has no miner
			// concept — blocks are produced by rotating validators chosen by
			// VRF. Return the current application's address as a harmless
			// non-zero placeholder so Solidity patterns that check
			// `coinbase != address(0)` still work.
			Logger::instance().warning(
				"block.coinbase has no AVM analog — returning CurrentApplicationAddress. "
				"EVM miner address is not a meaningful concept on Algorand.", _loc);
			call->opCode = "global";
			call->immediates = {std::string("CurrentApplicationAddress")};
			call->wtype = awst::WType::accountType();
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
		else if (_memberName == "gasprice")
		{
			// tx.gasprice → txn Fee (in microAlgos).
			// WARNING: These are NOT equivalent. EVM gas price is per-unit cost
			// (wei/gas) used for gas accounting and MEV protection. AVM txn Fee
			// is the flat fee (in microAlgos) attached to the transaction —
			// typically 1000 microAlgos. There is no per-opcode pricing on AVM.
			Logger::instance().warning(
				"tx.gasprice mapped to txn Fee (microAlgos). "
				"NOT equivalent to EVM gas price: AVM uses a flat transaction fee "
				"(typically 1000 microAlgos), not a per-opcode gas price.", _loc);
			call->opCode = "txn";
			call->immediates = {std::string("Fee")};
			call->wtype = awst::WType::uint64Type();
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
