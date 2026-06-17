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
		// block.chainid: handled by SolIntrinsicAccess before this mapper.
		// Prior dead handler returned GenesisHash (wrong type); removed.
		if (_memberName == "coinbase")
		{
			// block.coinbase: no AVM analog (blocks by VRF validators, not miners).
			// Return CurrentApplicationAddress as harmless non-zero placeholder.
			Logger::instance().warning(
				"block.coinbase has no AVM analog — returning CurrentApplicationAddress. "
				"EVM miner address is not a meaningful concept on Algorand.", _loc);
			return awst::makeGlobal("CurrentApplicationAddress", awst::WType::accountType(), _loc);
		}
	}
	else if (_objectName == "tx")
	{
		if (_memberName == "origin")
		{
			// tx.origin → hard error. AVM has no origin concept; it would silently alias
			// msg.sender, making `tx.origin == msg.sender` always-true (access-control
			// inversion). Refuse to compile rather than emit a vacuous guard.
			Logger::instance().error(
				"`tx.origin` is not supported on AVM. It denotes the EOA that "
				"started the transaction, distinct from `msg.sender`; AVM has no "
				"such concept, so `tx.origin` would silently alias `msg.sender` "
				"and make `tx.origin (==|!=) msg.sender` access checks vacuous. "
				"Use `msg.sender` (which maps correctly to `txn Sender`).", _loc);
			// Stub so build completes; error above aborts before TEAL emit.
			return awst::makeTxn("Sender", awst::WType::accountType(), _loc);
		}
		if (_memberName == "gasprice")
		{
			// tx.gasprice → txn Fee (microAlgos). NOT equivalent: EVM gas price is
			// per-opcode; AVM Fee is a flat ~1000 microAlgo transaction fee.
			Logger::instance().warning(
				"tx.gasprice mapped to txn Fee (microAlgos). "
				"NOT equivalent to EVM gas price: AVM uses a flat transaction fee "
				"(typically 1000 microAlgos), not a per-opcode gas price.", _loc);
			return awst::makeTxn("Fee", awst::WType::uint64Type(), _loc);
		}
	}

	return nullptr;
}

} // namespace puyasol::builder
