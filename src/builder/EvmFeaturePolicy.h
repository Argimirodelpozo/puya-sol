#pragma once

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

} // namespace puyasol::builder
