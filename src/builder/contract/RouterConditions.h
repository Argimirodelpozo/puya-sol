#pragma once

/// @file RouterConditions.h
/// The two transaction-shape conditions every entry router arm guards on.
/// SelectorRouter (ARC-4 dispatch) and EvmEntryRouter (EVM selector arms, both
/// profiles) each carried a private copy; the nodes built are identical —
/// `makeZero(loc)` and `makeIntegerConstant(0, loc)` are the same uint64
/// constant — so the copies only invited drift.

#include "awst/Node.h"

#include <memory>

namespace puyasol::builder
{

/// Txn.OnCompletion == NoOp(0).
inline std::shared_ptr<awst::Expression> isNoOpCall(awst::SourceLocation const& _loc)
{
	return awst::makeNumericCompare(
		awst::makeTxn("OnCompletion", awst::WType::uint64Type(), _loc),
		awst::NumericComparison::Eq, awst::makeZero(_loc), _loc);
}

/// Txn.NumAppArgs == `_count`.
inline std::shared_ptr<awst::Expression> appArgCountIs(
	uint64_t _count, awst::SourceLocation const& _loc)
{
	return awst::makeNumericCompare(
		awst::makeTxn("NumAppArgs", awst::WType::uint64Type(), _loc),
		awst::NumericComparison::Eq,
		awst::makeIntegerConstant(_count, _loc), _loc);
}

/// msg.value on AVM: the immediately-preceding payment's Amount, or 0 when
/// this call leads the group — `GroupIndex > 0 ? gtxns[GroupIndex-1].Amount
/// : 0`. uint64; the msg.value intrinsic promotes to biguint (uint256 view),
/// the non-payable guards assert it is zero. Was built by hand at three
/// sites (ContractBuilder guard, EVM-entry __evm_npy, the intrinsic).
inline std::shared_ptr<awst::Expression> makeMsgValueAmount(
	awst::SourceLocation const& _loc)
{
	auto hasPayment = awst::makeNumericCompare(
		awst::makeTxn("GroupIndex", awst::WType::uint64Type(), _loc),
		awst::NumericComparison::Gt, awst::makeZero(_loc), _loc);
	auto payIdx = awst::makeUInt64BinOp(
		awst::makeTxn("GroupIndex", awst::WType::uint64Type(), _loc),
		awst::UInt64BinaryOperator::Sub, awst::makeOne(_loc), _loc);
	auto amount = awst::makeGtxns(
		"Amount", std::move(payIdx), awst::WType::uint64Type(), _loc);
	return awst::makeConditional(
		std::move(hasPayment), std::move(amount), awst::makeZero(_loc),
		awst::WType::uint64Type(), _loc);
}

} // namespace puyasol::builder
