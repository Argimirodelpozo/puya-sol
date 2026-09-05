#include "builder/EvmFeaturePolicy.h"

#include "Logger.h"

#include <array>
#include <string>

namespace puyasol::builder
{

namespace
{

struct AllowableDivergence
{
	EvmFeature feature;
	std::string_view name;
};

constexpr std::array<AllowableDivergence, 15> allowableDivergences{
	AllowableDivergence{EvmFeature::BlockChainId, "block-chainid"},
	AllowableDivergence{EvmFeature::BlockDifficulty, "block-difficulty"},
	AllowableDivergence{EvmFeature::BlockBaseFee, "block-basefee"},
	AllowableDivergence{EvmFeature::BlockBlobBaseFee, "block-blobbasefee"},
	AllowableDivergence{EvmFeature::BlockGasLimit, "block-gaslimit"},
	AllowableDivergence{EvmFeature::BlockPrevrandao, "block-prevrandao"},
	AllowableDivergence{EvmFeature::TxGasPrice, "tx-gasprice"},
	AllowableDivergence{EvmFeature::AddressBalance, "address-balance-units"},
	AllowableDivergence{EvmFeature::GasLeft, "gasleft"},
	AllowableDivergence{EvmFeature::StaticCall, "staticcall"},
	AllowableDivergence{EvmFeature::DelegateCall, "delegatecall"},
	AllowableDivergence{EvmFeature::LowLevelCallOutcome, "low-level-call-outcome"},
	AllowableDivergence{EvmFeature::NativeValueTransfer, "native-value-transfer"},
	AllowableDivergence{EvmFeature::SelfCall, "self-call"},
	AllowableDivergence{EvmFeature::TryCatch, "try-catch"},
};

} // namespace

EvmFeatureDecision EvmFeaturePolicy::decide(
	EvmFeature _feature, TargetProfile const& _profile)
{
	using F = EvmFeatureFidelity;
	switch (_feature)
	{
	case EvmFeature::BlockChainId:
		if (_profile.evmChainId)
			return {F::ConfiguredEnvironment, "block.chainid",
				"uses the compile-time --evm-chain-id value"};
		return {F::AvmAdaptation, "block.chainid",
			"uses the Algorand GenesisHash interpreted as a uint256 network identity; "
			"set --evm-chain-id for an EVM-compatible numeric domain"};
	case EvmFeature::BlockDifficulty:
		return {F::AvmAdaptation, "block.difficulty",
			"post-Paris EVM defines difficulty as prevrandao (same opcode); "
			"lowered identically to the Algorand block seed for Round - 2"};
	case EvmFeature::BlockBaseFee:
		return {F::AvmAdaptation, "block.basefee",
			"returns zero because AVM has no EIP-1559 base fee"};
	case EvmFeature::BlockBlobBaseFee:
		return {F::AvmAdaptation, "block.blobbasefee",
			"returns zero because AVM has no blob fee market"};
	case EvmFeature::BlockGasLimit:
		if (_profile.evmBlockGasLimit)
			return {F::ConfiguredEnvironment, "block.gaslimit",
				"uses the compile-time --evm-block-gas-limit value"};
		return {F::AvmAdaptation, "block.gaslimit",
			"uses the group's total pooled app-call opcode budget "
			"(GroupSize x 700) — constant within an execution like EVM's "
			"block-level value; set --evm-block-gas-limit for an exact "
			"EVM number"};
	case EvmFeature::BlockPrevrandao:
		return {F::AvmAdaptation, "block.prevrandao",
			"uses the Algorand block seed for Round - 2"};
	case EvmFeature::BlockCoinbase:
		if (_profile.evmCoinbase)
			return {F::ConfiguredEnvironment, "block.coinbase",
				"uses the compile-time --evm-coinbase address"};
		return {F::HardCompileError, "block.coinbase",
			"Algorand exposes no EVM miner/fee-recipient address; configure one "
			"explicitly with --evm-coinbase"};
	case EvmFeature::TxOrigin:
		return {F::HardCompileError, "tx.origin",
			"AVM has no transaction-origin value distinct from msg.sender"};
	case EvmFeature::TxGasPrice:
		return {F::AvmAdaptation, "tx.gasprice",
			"uses txn Fee in microAlgos; AVM fees are not per-opcode gas prices"};
	case EvmFeature::AddressBalance:
		return {F::AvmAdaptation, "address.balance",
			"returns microAlgos rather than wei; contract accounting must use AVM units"};
	case EvmFeature::GasLeft:
		return {F::AvmAdaptation, "gasleft()",
			"uses the current AVM OpcodeBudget"};
	case EvmFeature::BlobHash:
		return {F::HardCompileError, "blobhash()",
			"AVM has no blob transactions or versioned blob hashes"};
	case EvmFeature::BlockHash:
		return {F::HardCompileError, "blockhash()",
			"AVM exposes a recent block seed, not an EVM block hash"};
	case EvmFeature::StaticCall:
		return {F::AvmAdaptation, "staticcall",
			"AVM does not enforce the EVM read-only guarantee; cross-contract "
			"static calls use ordinary inner application calls and may change state"};
	case EvmFeature::DelegateCall:
		return {F::HardRuntimeFailure, "address.delegatecall",
			"AVM has neither shared caller storage nor EVM delegate-call context"};
	case EvmFeature::LowLevelCallOutcome:
		return {F::AvmAdaptation, "low-level call result",
			"returns true only after a submitted AVM inner transaction succeeds; "
			"a rejected inner transaction aborts the outer call, so false is not catchable"};
	case EvmFeature::NativeValueTransfer:
		return {F::AvmAdaptation, "native value transfer",
			"sends microAlgos to a keyless padded 160-bit identity unless an "
			"xchain account template is configured; such funds are unrecoverable"};
	case EvmFeature::SelfCall:
		return {F::AvmAdaptation, "external self-call",
			"uses internal dispatch because AVM forbids app reentrancy; msg.sender, "
			"msg.value, and revert-isolation behavior differ from EVM"};
	case EvmFeature::TryCatch:
		return {F::AvmAdaptation, "try/catch",
			"drops catch clauses because a failed AVM inner transaction aborts the "
			"outer transaction and cannot be caught"};
	case EvmFeature::UnknownLowLevelCall:
		return {F::HardCompileError, "unresolved low-level call",
			"the target/calldata cannot be proven to match an AVM application route; "
			"fabricating (true, empty-bytes) would make error handling pass spuriously"};
	case EvmFeature::LibraryAddress:
		return {F::HardCompileError, "address(library)",
			"Solidity libraries are inlined as AVM subroutines and have no deployed "
			"application identity; fabricating address(0) would corrupt identity checks"};
	case EvmFeature::CreationCode:
		return {F::HardCompileError, "type(C).creationCode",
			"EVM bytecode does not describe the deployed AVM application; its "
			"consumers (CREATE2 address derivation, code hashing) would compute "
			"values that correspond to nothing on chain"};
	case EvmFeature::RuntimeCode:
		return {F::HardCompileError, "type(C).runtimeCode",
			"EVM bytecode does not describe the deployed AVM application; its "
			"consumers (CREATE2 address derivation, code hashing) would compute "
			"values that correspond to nothing on chain"};
	}
	return {F::HardCompileError, "unknown EVM feature",
		"has no declared AVM lowering policy"};
}

std::string_view EvmFeaturePolicy::allowName(EvmFeature _feature)
{
	for (auto const& candidate: allowableDivergences)
		if (candidate.feature == _feature)
			return candidate.name;
	return {};
}

bool EvmFeaturePolicy::isAllowName(std::string_view _name)
{
	for (auto const& candidate: allowableDivergences)
		if (candidate.name == _name)
			return true;
	return false;
}

std::string EvmFeaturePolicy::allowedNames()
{
	std::string result;
	for (auto const& candidate: allowableDivergences)
	{
		if (!result.empty())
			result += ", ";
		result += candidate.name;
	}
	return result;
}

void EvmFeaturePolicy::report(
	EvmFeature _feature,
	TargetProfile const& _profile,
	awst::SourceLocation const& _loc)
{
	auto const decision = decide(_feature, _profile);
	if (decision.fidelity == EvmFeatureFidelity::Exact)
		return;

	auto const allow = allowName(_feature);
	// Static-call write protection is an accepted divergence: warn without
	// requiring an opt-in. Keep its old CLI token valid for existing callers.
	bool const needsOptIn = _feature != EvmFeature::StaticCall
		&& (decision.fidelity == EvmFeatureFidelity::AvmAdaptation
			|| decision.fidelity == EvmFeatureFidelity::HardRuntimeFailure);
	bool const explicitlyAllowed = needsOptIn && !allow.empty()
		&& _profile.allowedEvmDivergences.contains(std::string(allow));

	std::string message = "[";
	if (needsOptIn && !explicitlyAllowed)
		message += "unapproved EVM divergence";
	else switch (decision.fidelity)
	{
	case EvmFeatureFidelity::Exact: break;
	case EvmFeatureFidelity::AvmAdaptation: message += "allowed AVM adaptation"; break;
	case EvmFeatureFidelity::ConfiguredEnvironment: message += "configured EVM environment"; break;
	case EvmFeatureFidelity::HardCompileError: message += "unsupported EVM feature"; break;
	case EvmFeatureFidelity::HardRuntimeFailure: message += "allowed runtime-unsupported EVM feature"; break;
	}
	message += ": " + std::string(decision.name) + "] "
		+ std::string(decision.explanation) + ".";
	if (needsOptIn && !explicitlyAllowed && !allow.empty())
		message += " Compilation is fail-closed; pass --allow-divergence "
			+ std::string(allow) + " to acknowledge this behavior.";
	else if (explicitlyAllowed)
		message += " Explicitly enabled by --allow-divergence "
			+ std::string(allow) + ".";

	if (decision.fidelity == EvmFeatureFidelity::HardCompileError
		|| (needsOptIn && !explicitlyAllowed))
		Logger::instance().error(message, _loc);
	else
		Logger::instance().warning(message, _loc);
}

} // namespace puyasol::builder
