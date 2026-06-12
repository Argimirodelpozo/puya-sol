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
		// block.chainid is handled by SolIntrinsicAccess (constant 1 +
		// warning) before this mapper runs — no handler here. A previous
		// dead handler returned bytes-typed GenesisHash, which is the wrong
		// type for chainid arithmetic; removed so a future handler-order
		// change cannot silently activate it.
		if (_memberName == "coinbase")
		{
			// block.coinbase is the miner address on EVM. AVM has no miner
			// concept — blocks are produced by rotating validators chosen by
			// VRF. Return the current application's address as a harmless
			// non-zero placeholder so Solidity patterns that check
			// `coinbase != address(0)` still work.
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
			// tx.origin → HARD ERROR. On EVM, tx.origin is the EOA that
			// started the whole transaction, distinct from msg.sender (the
			// immediate caller). AVM has no transaction-origin concept; the
			// only available value is `txn Sender`, which equals msg.sender —
			// so tx.origin would silently alias msg.sender. That makes
			// `tx.origin == msg.sender` (the classic "reject contract callers"
			// guard) ALWAYS true and `tx.origin != msg.sender` always false,
			// silently inverting access-control logic. Refuse to compile
			// rather than emit a vacuous guard. (msg.sender itself IS sound:
			// `txn Sender` is the correct AVM analog of the immediate caller.)
			Logger::instance().error(
				"`tx.origin` is not supported on AVM. It denotes the EOA that "
				"started the transaction, distinct from `msg.sender`; AVM has no "
				"such concept, so `tx.origin` would silently alias `msg.sender` "
				"and make `tx.origin (==|!=) msg.sender` access checks vacuous. "
				"Use `msg.sender` (which maps correctly to `txn Sender`).", _loc);
			// Stub so AWST building completes; the error above aborts the build
			// before any TEAL is emitted (same pattern as create2/delegatecall).
			return awst::makeTxn("Sender", awst::WType::accountType(), _loc);
		}
		if (_memberName == "gasprice")
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
			return awst::makeTxn("Fee", awst::WType::uint64Type(), _loc);
		}
	}

	return nullptr;
}

} // namespace puyasol::builder
