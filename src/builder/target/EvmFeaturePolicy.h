#pragma once

#include "HexBytes.h"

#include <cctype>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "awst/SourceLocation.h"
#include "builder/target/TargetProfile.h"

#include <string_view>

namespace puyasol::awst { struct Expression; }

namespace puyasol::builder
{
class TypeMapper;

/// Solidity and Yul share the selected address namespace and verified claim.
std::shared_ptr<awst::Expression> buildMessageSender(TypeMapper&, awst::SourceLocation const&);

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
	AddressBalance,
	GasLeft,
	BlobHash,
	BlockHash,
	StaticCall,
	DelegateCall,
	LowLevelCallOutcome,
	NativeValueTransfer,
	SelfCall,
	TryCatch,
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

	/// Stable CLI token for an adaptation that can be explicitly acknowledged.
	/// Empty means the feature is not eligible for an opt-in.
	static std::string_view allowName(EvmFeature _feature);
	static bool isAllowName(std::string_view _name);
	static std::string allowedNames();

	/// Emit the centrally-owned diagnostic for a non-exact decision. Exact
	/// features are silent; opt-in-eligible behavior is an error unless its
	/// allowName() is present in TargetProfile::allowedEvmDivergences. StaticCall
	/// is an accepted divergence and only warns, with or without its legacy opt-in.
	static void report(
		EvmFeature _feature,
		TargetProfile const& _profile,
		awst::SourceLocation const& _loc);
};

/// One opted-in seed mapping for Solidity and Yul: FirstValid - 1, or zero
/// at FirstValid 0. Predictable/caller-selectable, not secure randomness.
std::shared_ptr<awst::Expression> buildBlockSeed(
	EvmFeature, TargetProfile const&, awst::SourceLocation const&);

/// 20-byte coinbase address from the profile's `--evm-coinbase` hex.
/// Case-insensitive, so the asm and Solidity lowerings can never diverge on
/// a producer that skips CliOptions' lowercasing (previously two verbatim
/// nibble-decoder copies).
inline std::vector<uint8_t> decodeEvmCoinbase20(std::string const& _hex)
{
	// One strict decoder for every hex CLI input (audit H-06). The previous
	// nibble loop indexed 40 characters without checking the length — an
	// out-of-bounds read on short input — and mapped a non-hex character to a
	// garbage nibble. `--evm-coinbase` is validated by CliOptions'
	// parseAddressHex; programmatic producers must also fail closed.
	if (auto bytes = hexToBytes(_hex, 20))
		return *bytes;
	throw std::invalid_argument("EVM coinbase must be exactly 20 hex-encoded bytes");
}

} // namespace puyasol::builder
