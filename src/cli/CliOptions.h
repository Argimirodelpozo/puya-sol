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
	std::string evmVersion;     // empty = compiler default (cancun)
	// --evm-memory-slots <N>: N scratch slots for EVM memory
	// (=> AssemblyBuilder::MEMORY_SLOT_LAST = N-1). 0 = leave the default
	// (5 slots = 20KB). Raise for memory-hungry contracts (UltraHonk => 32).
	int evmMemorySlots = 0;
	// SimpleSplitter (alternative to UrosSplitter): static "extract-named-
	// subroutines" splitter ported from polymarket-experiment. Moves whole
	// subroutines from the primary contract into a sibling helper contract
	// (template-var TMPL_<helperName>_APP_ID for app-id substitution). Used
	// by polymarket v1+v2's compile_all.sh to keep CTFExchange under the
	// 8 KB cap. Side-effect: also avoids the puya address-param→uint64
	// over-elision bug in inner-call ApplicationID (puyabug.md #5) because
	// the extraction changes the orch's stack layout.
	std::string splitConfig;       // --split-config <json-path>
	std::vector<std::string> forceDelegate; // --force-delegate <names>

	// --pin-to-main: methods that MUST stay on the main contract and never
	// be moved into a uros chunk. The validator below rejects any
	// --uros-splitter group that includes one of these names. Reasons a
	// method needs to be pinned:
	//
	//   * Reads `msg.sender`. Inside a chunk body the call frame is an
	//     itxn issued by the orch, so `Txn.Sender` resolves to orch's app
	//     account instead of the user's address — every auth check, every
	//     `_balances[msg.sender]` lookup, every `transferFrom(msg.sender,
	//     ...)` silently misbehaves. Until sender forwarding lands in the
	//     orch dance (args[0] packing or similar), the only safe option is
	//     to keep these methods unsplit.
	//
	//   * Reads `address(this)` AND callers expect the result to match
	//     main's address. Inside a chunk the runtime "this" is the
	//     __storage app account, not main, so any external contract that
	//     identifies the contract by address will misbehave.
	//
	// This flag does NOT auto-detect — it's a manual list. The compile
	// script is responsible for naming the methods it wants pinned. A
	// future iteration can add call-graph-based auto-detection on top.
	std::vector<std::string> pinnedToMain;

	// --fn-split <Name>:<idx>,<idx>,...:g<N>[:cross]
	//
	// Slice the named subroutine / contract method's body into N+1
	// pieces along the statement indices. The optional `:cross` suffix
	// marks the chain as cross-chunk: pieces are intended to live on
	// SEPARATE uros chunks and run as siblings inside one staged
	// inner-txn group orchestrated by orch.dispatch_chain. Without
	// `:cross`, pieces share the same txn frame (load 100); with it,
	// they read each other via gload <prev_idx> 100.
	//
	// Repeatable: one flag invocation per target to split.
	struct FnSplitSpec
	{
		std::string subroutineName;
		std::vector<size_t> splitPoints;
		int groupId = 0;
		bool crossChunk = false;
	};
	std::vector<FnSplitSpec> fnSplits;

	// --deploy-pure-helpers
	//
	// Lift every pure Subroutine (state-mutability `pure` in Solidity)
	// into its own one-method sidecar Contract; rewrite the call sites
	// in the rest of the contract set to inner-txn ApplicationCall the
	// helper. Removes the helper's bytecode from the calling chunks
	// (per-Contract DCE drops the now-unreached Subroutine).
	bool deployPureHelpers = false;

	// --force-inline-sub <Name>
	//
	// Set inlineOpt=true on every Subroutine OR ContractMethod whose
	// memberName/name matches. Puya's inliner expands the body at every
	// call site instead of emitting a callsub, so the subroutine itself
	// can be DCE'd from chunks where it would otherwise show up as a
	// reachable internal.
	//
	// Useful for breaking subroutine-reachability closures that bloat
	// FunctionSplitter chunks (e.g. inlining a heavy non-pure helper
	// like `_calculateUserAccountData` so its body can be sliced via
	// `--fn-split` rather than dragged in whole). Repeatable.
	std::vector<std::string> forceInlineSubs;
	// --force-no-inline-sub: the inverse — set inlineOpt=false so a single-call
	// subroutine stays a real callsub (NOT inlined into its caller). Needed to
	// expose phase functions as slice boundaries for --fn-split. Repeatable.
	std::vector<std::string> forceNoInlineSubs;

	// --pure-helper-split <SubName>:<idx>,<idx>,...
	//
	// Slice the named pure subroutine's body at the given statement
	// indices BEFORE lifting it via --deploy-pure-helpers. With N
	// indices the body splits into N+1 pieces; PureHelperExtractor
	// builds one sidecar Contract per piece (so a 14 KB Sub split
	// at one point yields two 7 KB sidecars, fitting the AVM 8 KB
	// per-program cap). Live vars across each split flow through
	// scratch slot 100 + `gload` from the previous inner txn.
	//
	// Repeatable: one flag invocation per Sub to split.
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
