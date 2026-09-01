#pragma once

/// @file XchainAccounts.h
/// The xchain account model (github.com/algorandfoundation/xchain-accounts):
/// a 20-byte EVM identity E owns the LogicSig account whose program is the
/// PINNED template with E spliced at the owner placeholder. The address is
/// on-chain computable — A(E) = sha512_256("Program" || prefix || E || suffix)
/// — which makes the EVM profile's 160-bit identities SPENDABLE: payments
/// route to A(E) instead of the keyless padded pseudo-account, and a caller
/// that IS A(E) can claim E as msg.sender (the entry arm verifies the hash).

#include "awst/Node.h"
#include "builder/TargetProfile.h"

#include <memory>
#include <vector>

namespace puyasol::builder::xchain
{

/// "Program" || programPrefix as one bytes constant (the lsig address domain
/// tag concatenated with the template bytes before the owner splice).
inline std::shared_ptr<awst::Expression> domainPrefixConst(
	TargetProfile::XchainAccounts const& _x, awst::SourceLocation const& _loc)
{
	std::vector<uint8_t> bytes{'P', 'r', 'o', 'g', 'r', 'a', 'm'};
	bytes.insert(bytes.end(), _x.programPrefix.begin(), _x.programPrefix.end());
	return awst::makeBytesConstant(std::move(bytes), _loc);
}

/// A(E) for a 20-byte owner expression: the derived LogicSig account.
inline std::shared_ptr<awst::Expression> derivedAccount(
	TargetProfile::XchainAccounts const& _x,
	std::shared_ptr<awst::Expression> _owner20,
	awst::SourceLocation const& _loc)
{
	auto program = awst::makeConcat(
		awst::makeConcat(
			domainPrefixConst(_x, _loc), std::move(_owner20), _loc),
		awst::makeBytesConstant(
			std::vector<uint8_t>(_x.programSuffix), _loc),
		_loc);
	auto hash = awst::makeIntrinsicCall(
		"sha512_256", awst::WType::bytesType(), _loc);
	hash->stackArgs.push_back(std::move(program));
	return awst::makeAsAccount(std::move(hash), _loc);
}

/// Payment-receiver mapping: a 160-bit pseudo-account (high 12 bytes zero,
/// and not the bzero24 ++ appId contract convention) pays its owner's derived
/// LogicSig account; every other receiver (real accounts, app escrows, the
/// contract convention) passes through untouched. Runtime conditional — the
/// receiver's shape is a value property.
inline std::shared_ptr<awst::Expression> mapPaymentReceiver(
	TargetProfile const& _profile,
	std::shared_ptr<awst::Expression> _receiver,
	awst::SourceLocation const& _loc)
{
	if (!_profile.xchainAccounts)
		return _receiver;
	auto const& x = *_profile.xchainAccounts;
	auto r = awst::makeEvalOnce(
		awst::makeAsBytes(std::move(_receiver), _loc), _loc);
	auto hi12Zero = awst::makeBytesComparison(
		awst::makeExtract(r, 0, 12, _loc),
		awst::EqualityComparison::Eq,
		awst::makeBzero(12, _loc), _loc);
	// bzero24 ++ appId (the contract-value convention) keeps its meaning: an
	// EVM identity with 12 leading zero BYTES is unconstructible in practice.
	auto notConvention = awst::makeBytesComparison(
		awst::makeExtract(r, 12, 12, _loc),
		awst::EqualityComparison::Ne,
		awst::makeBzero(12, _loc), _loc);
	auto isEvmIdentity = awst::makeBoolBinOp(
		std::move(hi12Zero), awst::BinaryBooleanOperator::And,
		std::move(notConvention), _loc);
	return awst::makeConditional(
		std::move(isEvmIdentity),
		derivedAccount(x, awst::makeExtract(r, 12, 20, _loc), _loc),
		awst::makeAsAccount(r, _loc),
		awst::WType::accountType(), _loc);
}

} // namespace puyasol::builder::xchain
