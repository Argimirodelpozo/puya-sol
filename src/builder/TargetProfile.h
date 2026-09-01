#pragma once

#include "builder/ScratchLayout.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

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
	/// xchain account model (github.com/algorandfoundation/xchain-accounts):
	/// each 20-byte EVM identity E owns the LogicSig account whose program is
	/// the pinned template with E spliced at the placeholder —
	/// A(E) = sha512_256("Program" || prefix || E || suffix). When set (EVM
	/// profile only): native value transfers to 160-bit identities pay A(E)
	/// instead of the keyless padded pseudo-account, and the entry arm accepts
	/// a VERIFIED owner claim (ApplicationArgs[2]) adopted as msg.sender.
	struct XchainAccounts
	{
		std::vector<uint8_t> programPrefix;
		std::vector<uint8_t> programSuffix;
	};
	std::optional<XchainAccounts> xchainAccounts;

	/// `new C()` child approval programs load from a "__cp_<Child>" box
	/// provisioned by the deployer (via the synthesized __provisionChildProg
	/// method) instead of being embedded as template constants in the parent
	/// bytecode. Shaves the child's full size off the parent program — the
	/// only road when parent + embedded child exceeds the 16KB program cap
	/// but the parent alone fits. Only children created in __postInit can use
	/// this (boxes cannot exist before the app does).
	bool childProgramsViaBox = false;

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
