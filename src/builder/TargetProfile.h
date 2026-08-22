#pragma once

#include "builder/ScratchLayout.h"

#include <optional>
#include <string>

namespace puyasol::builder
{

enum class ContractAbi
{
	Arc4,
	Evm,
};

/// Target choices and unit-wide storage facts for one compiler invocation.
/// Builder services receive this through TypeMapper instead of consulting
/// mutable process-global switches.
struct TargetProfile
{
	bool evmStorageLayout = false;
	bool evmMemoryLayout = false;
	bool evmSelectors = false;
	/// Wire protocol at the AVM application entry/return boundary. This does
	/// not alter Solidity `abi.*` expression semantics. The EVM profile also
	/// normalises ambient address identities (e.g. msg.sender/caller()) to the
	/// same 160-bit namespace used by decoded address arguments.
	ContractAbi contractAbi = ContractAbi::Arc4;
	bool viaIRSequencing = false;
	bool denseOnlyStorage = false;
	bool singlePageStorage = false;
	/// Explicit EVM environment inputs. Decimal uint256 strings are retained
	/// losslessly; coinbase is a normalized 40-hex-digit Solidity address.
	std::optional<std::string> evmChainId;
	std::optional<std::string> evmBlockGasLimit;
	std::optional<std::string> evmCoinbase;
	/// Canonical EVMVersion::name(); empty means the compiler default.
	/// Stored as the NAME, not the value: a by-value EVMVersion made this
	/// 44-line POD drag liblangutil/EVMVersion.h -> boost.exception into
	/// 122 of 139 builder TUs, while exactly one of them reads the field.
	/// EVMVersion::fromString compares against name(), so this round-trips
	/// exactly.
	std::string evmVersionName;
	ScratchLayout scratchLayout;
};

} // namespace puyasol::builder
