/// @file CliOptions.h
/// puya-sol CLI surface: the Options struct, --help text, argv parsing, and
/// Logger configuration. Moved out of main.cpp so the driver stays a readable
/// pipeline; splitter-specific option *application* lives in AwstPostPasses.
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace puyasol::cli
{

struct Options
{
	std::vector<std::string> sourceFiles;
	std::vector<std::string> importPaths;
	std::vector<std::string> remappings;
	std::string outputDir = "out";
	std::string puyaPath;
	std::string logLevel = "info";
	bool dumpAwst = false;
	bool noPuya = false;
	uint64_t opupBudget = 0;
	std::map<std::string, uint64_t> ensureBudget; // func_name → budget
	int optimizationLevel = 2;
	bool outputIr = false;
	bool outputLogs = true;
	bool viaYulBehavior = false;
	// --evm-selectors: expose Solidity/EVM keccak selectors to Solidity code
	// while retaining ARC-4 selectors at the AVM application-call boundary.
	// Opt-in because external function-pointer representation grows to carry
	// both identities and observable selector values change.
	bool evmSelectors = false;
	std::string contractAbi = "arc4";
	std::string evmVersion;     // empty = compiler default (cancun)
	// Explicit EVM environment inputs used by the semantic policy. Empty means
	// use the documented AVM adaptation (chain id / gas limit) or fail where no
	// honest adaptation exists (coinbase).
	std::string evmChainId;
	std::string evmBlockGasLimit;
	std::string evmCoinbase;
	/// Repeatable --allow-divergence names validated by EvmFeaturePolicy.
	std::set<std::string> allowedEvmDivergences;
	// --xchain-template <hex>: compiled xchain LogicSig template bytecode
	// containing a 20-byte owner placeholder (--xchain-placeholder, default
	// 20x 0xee). Enables the xchain account model in the EVM profile: native
	// value transfers to 160-bit identities route to the owner's derived
	// LogicSig account sha512_256("Program" || template-with-owner-spliced),
	// and the entry arm accepts a VERIFIED owner claim as msg.sender. The
	// template must be PINNED — the derived address is the exact program hash.
	std::string xchainTemplateHex;
	std::string xchainPlaceholderHex = "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";
	// --child-programs-via-box: `new C()` child approval programs load from a
	// deployer-provisioned "__cp_<Child>" box instead of being embedded as
	// template constants — for parents that only fit the 16KB program cap
	// without the embedded child bytes.
	bool childProgramsViaBox = false;
	// --evm-memory-slots <N>: 0 means unspecified/default (5 slots=20KB).
	// Pages are contiguous from slot 0; the transient blob sits at N and the
	// AVM.sol group-scratch range at N+1..N+10 (default N=5 reproduces the
	// historical 0..4 / 5 / 6..15 layout exactly). UltraHonk needs ~32.
	int evmMemorySlots = 0;
	// --evm-memory-layout: universal blob memory — every asm-touched memory
	// aggregate is pointer-modeled (EVM layout) regardless of allocation shape.
	bool evmMemoryLayout = false;
	// --evm-storage-layout: back ALL contract storage with EVM-numbered slots
	// (hybrid paged/sparse boxes) instead of per-variable named cells. Makes
	// assembly slot arithmetic faithful; disables ARC-56 state declarations.
	bool evmStorageLayout = false;
	// --force-inline-sub <Name>: set inlineOpt=true so puya inlines at every
	// call site. Repeatable.
	std::vector<std::string> forceInlineSubs;
	// --force-no-inline-sub: set inlineOpt=false to retain a real subroutine.
	// Repeatable.
	std::vector<std::string> forceNoInlineSubs;
};

void printUsage(char const* _progName);

Options parseArgs(int _argc, char* _argv[]);

/// Configure the global Logger from --log-level + --output-dir options.
void configureLogger(Options const& _opts);

} // namespace puyasol::cli
