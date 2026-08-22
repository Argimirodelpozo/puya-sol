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

} // namespace puyasol::builder
