/// @file CliOptions.h
/// puya-sol CLI surface: the Options struct, --help text, argv parsing, and
/// Logger configuration. Moved out of main.cpp so the driver stays a readable
/// pipeline; splitter-specific option *application* lives in AwstPostPasses.
#pragma once

#include <cstdint>
#include <map>
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
	// --xchain-template <hex>: compiled xchain LogicSig template bytecode
	// containing a 20-byte owner placeholder (--xchain-placeholder, default
	// 20x 0xee). Enables the xchain account model in the EVM profile: native
	// value transfers to 160-bit identities route to the owner's derived
	// LogicSig account sha512_256("Program" || template-with-owner-spliced),
	// and the entry arm accepts a VERIFIED owner claim as msg.sender. The
	// template must be PINNED — the derived address is the exact program hash.
	std::string xchainTemplateHex;
	std::string xchainPlaceholderHex = "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";
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
	// SimpleSplitter moves pure subroutines into sibling helper apps. Its
	// delegate mode compiles externally routable methods as state-preserving
	// code pages which execute on the original app after UpdateApplication.
	std::string splitConfig;       // --split-config <json-path>
	std::vector<std::string> forceDelegate; // --force-delegate <names>

	// --pin-to-main: methods that must not be moved into an ordinary sidecar. Pin if:
	//   * Reads msg.sender — inside a chunk Txn.Sender = orch's app account,
	//     not the user; every auth check / balances lookup silently misbehaves.
	//   * Reads address(this) expecting main's address — chunks see __storage.
	// Manual list; no auto-detection.
	std::vector<std::string> pinnedToMain;

	// --fn-split <Name>:<idx>,...:g<N>[:cross]
	// Slice subroutine body into N+1 pieces at statement indices.
	// Without :cross, pieces share the same txn frame (scratch 100).
	// With :cross, pieces live on separate chunks and pass state
	// via gload <prev_idx> 100. Repeatable.
	struct FnSplitSpec
	{
		std::string subroutineName;
		std::vector<size_t> splitPoints;
		int groupId = 0;
		bool crossChunk = false;
	};
	std::vector<FnSplitSpec> fnSplits;

	// --deploy-pure-helpers: lift each `pure` Subroutine into a one-method
	// sidecar Contract; rewrite call sites to inner-txn ApplicationCall.
	// DCE drops the subroutine from calling chunks.
	bool deployPureHelpers = false;

	// --force-inline-sub <Name>: set inlineOpt=true so puya inlines at every
	// call site (body can then be DCE'd from chunks). Useful for breaking
	// reachability closures (e.g. inline `_calculateUserAccountData` so it
	// can be sliced via --fn-split). Repeatable.
	std::vector<std::string> forceInlineSubs;
	// --force-no-inline-sub: set inlineOpt=false to keep a real callsub as a
	// --fn-split slice boundary. Repeatable.
	std::vector<std::string> forceNoInlineSubs;

	// --pure-helper-split <SubName>:<idx>,...: slice a pure subroutine before
	// --deploy-pure-helpers. N indices → N+1 sidecar Contracts (e.g. 30 KiB →
	// two 15 KiB programs). Live vars cross splits via scratch 100 +
	// gload. Repeatable.
	struct PureHelperSplitSpec
	{
		std::string subroutineName;
		std::vector<size_t> splitPoints;
	};
	std::vector<PureHelperSplitSpec> pureHelperSplits;
};

void printUsage(char const* _progName);

Options parseArgs(int _argc, char* _argv[]);

/// Configure the global Logger from --log-level + --output-dir options.
void configureLogger(Options const& _opts);

} // namespace puyasol::cli
