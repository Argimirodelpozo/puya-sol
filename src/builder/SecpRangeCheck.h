#pragma once

/// @file SecpRangeCheck.h
/// The secp256k1 scalar-range condition every ecrecover lowering must gate
/// `ecdsa_pk_recover` behind.
///
/// EVM's ecrecover precompile returns EMPTY output for r ∉ [1, N-1] or
/// s ∉ [1, N-1] (N = secp256k1 group order); AVM's ecdsa_pk_recover PANICS on
/// the same inputs. Three lowerings share the divergence surface — the
/// `ecrecover(...)` builtin, the assembly `staticcall(g, 1, ...)` handler, and
/// the Solidity-level `address(0x1).staticcall(...)` shape — so the condition
/// lives here once. Signature checks are attacker-fed by construction: an
/// out-of-range (r, s) must fail the check like any wrong signature, not
/// hard-revert the contract.

#include "awst/Node.h"

#include <functional>
#include <memory>

namespace puyasol::builder
{

/// secp256k1 group order N.
inline char const* secp256k1GroupOrder()
{
	return "115792089237316195423570985008687907852837564279074904382605163141518161494337";
}

/// r ∈ [1, N-1] && s ∈ [1, N-1]. `readR`/`readS` must return a fresh 32-byte
/// expression per call (each operand is read twice); the caller owns pinning.
inline std::shared_ptr<awst::Expression> secp256k1RangeCondition(
	std::function<std::shared_ptr<awst::Expression>()> const& readR,
	std::function<std::shared_ptr<awst::Expression>()> const& readS,
	awst::SourceLocation const& loc)
{
	// operand != 0 && operand < N
	auto inRange = [&](auto const& read) {
		auto nonZero = awst::makeNumericCompare(
			awst::makeAsBiguint(read(), loc), awst::NumericComparison::Ne,
			awst::makeIntegerConstant("0", loc, awst::WType::biguintType()),
			loc);
		auto belowN = awst::makeNumericCompare(
			awst::makeAsBiguint(read(), loc), awst::NumericComparison::Lt,
			awst::makeIntegerConstant(
				secp256k1GroupOrder(), loc, awst::WType::biguintType()),
			loc);
		return awst::makeBoolBinOp(std::move(nonZero),
			awst::BinaryBooleanOperator::And, std::move(belowN), loc);
	};
	return awst::makeBoolBinOp(inRange(readR),
		awst::BinaryBooleanOperator::And, inRange(readS), loc);
}

} // namespace puyasol::builder
