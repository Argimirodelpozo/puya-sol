#include "builder/EvmFeaturePolicy.h"

#include "Logger.h"

#include <string>

namespace puyasol::builder
{

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
		return {F::AvmAdaptation, "address.staticcall",
			"uses an ordinary inner application call; AVM cannot enforce the EVM "
			"read-only guarantee"};
	case EvmFeature::DelegateCall:
		return {F::HardRuntimeFailure, "address.delegatecall",
			"AVM has neither shared caller storage nor EVM delegate-call context"};
	case EvmFeature::LowLevelCallOutcome:
		return {F::AvmAdaptation, "low-level call result",
			"returns true only after a submitted AVM inner transaction succeeds; "
			"a rejected inner transaction aborts the outer call, so false is not catchable"};
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

void EvmFeaturePolicy::report(
	EvmFeature _feature,
	TargetProfile const& _profile,
	awst::SourceLocation const& _loc)
{
	auto const decision = decide(_feature, _profile);
	if (decision.fidelity == EvmFeatureFidelity::Exact)
		return;

	std::string message = "[";
	switch (decision.fidelity)
	{
	case EvmFeatureFidelity::Exact: break;
	case EvmFeatureFidelity::AvmAdaptation: message += "AVM adaptation"; break;
	case EvmFeatureFidelity::ConfiguredEnvironment: message += "configured EVM environment"; break;
	case EvmFeatureFidelity::HardCompileError: message += "unsupported EVM feature"; break;
	case EvmFeatureFidelity::HardRuntimeFailure: message += "runtime-unsupported EVM feature"; break;
	}
	message += ": " + std::string(decision.name) + "] "
		+ std::string(decision.explanation) + ".";

	if (decision.fidelity == EvmFeatureFidelity::HardCompileError)
		Logger::instance().error(message, _loc);
	else
		Logger::instance().warning(message, _loc);
}

} // namespace puyasol::builder
