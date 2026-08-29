#pragma once

#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

#include "awst/SourceLocation.h"
#include "builder/TargetProfile.h"

#include <string_view>

namespace puyasol::builder
{

/// Observable EVM semantics that need an explicit fidelity decision when
/// targeting AVM.  Keeping this list centralized prevents lowering sites from
/// quietly inventing plausible constants or successful results.
enum class EvmFeature
{
	BlockChainId,
	BlockDifficulty,
	BlockBaseFee,
	BlockBlobBaseFee,
	BlockGasLimit,
	BlockPrevrandao,
	BlockCoinbase,
	TxOrigin,
	TxGasPrice,
	GasLeft,
	BlobHash,
	BlockHash,
	StaticCall,
	DelegateCall,
	LowLevelCallOutcome,
	UnknownLowLevelCall,
	LibraryAddress,
	CreationCode,
	RuntimeCode,
};

enum class EvmFeatureFidelity
{
	Exact,
	AvmAdaptation,
	ConfiguredEnvironment,
	HardCompileError,
	HardRuntimeFailure,
};

struct EvmFeatureDecision
{
	EvmFeatureFidelity fidelity;
	std::string_view name;
	std::string_view explanation;
};

class EvmFeaturePolicy
{
public:
	/// Resolve the unit's single policy decision for `_feature`.
	static EvmFeatureDecision decide(
		EvmFeature _feature, TargetProfile const& _profile);

	/// Emit the centrally-owned diagnostic for a non-exact decision. Exact
	/// features are intentionally silent.
	static void report(
		EvmFeature _feature,
		TargetProfile const& _profile,
		awst::SourceLocation const& _loc);
};


/// 20-byte coinbase address from the profile's `--evm-coinbase` hex.
/// Case-insensitive, so the asm and Solidity lowerings can never diverge on
/// a producer that skips CliOptions' lowercasing (previously two verbatim
/// nibble-decoder copies).
inline std::vector<uint8_t> decodeEvmCoinbase20(std::string const& _hex)
{
	auto nibble = [](char c) -> uint8_t {
		if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
		return static_cast<uint8_t>(std::tolower(
			static_cast<unsigned char>(c)) - 'a' + 10);
	};
	std::vector<uint8_t> bytes(20);
	for (size_t i = 0; i < bytes.size(); ++i)
		bytes[i] = static_cast<uint8_t>(
			(nibble(_hex[2 * i]) << 4) | nibble(_hex[2 * i + 1]));
	return bytes;
}

} // namespace puyasol::builder
