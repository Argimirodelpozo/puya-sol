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
	// --evm-memory-slots <N>: sets MEMORY_SLOT_LAST=N-1. 0=default (5 slots=20KB).
	// UltraHonk needs 32.
	int evmMemorySlots = 0;
	// SimpleSplitter (alternative to UrosSplitter): moves subroutines into a
	// sibling helper contract (TMPL_<helperName>_APP_ID substitution). Used by
	// polymarket v1+v2 to keep CTFExchange under 8 KB. Also avoids puyabug.md
	// #5 (address-param→uint64 over-elision) due to changed orch stack layout.
	std::string splitConfig;       // --split-config <json-path>
	std::vector<std::string> forceDelegate; // --force-delegate <names>

	// --pin-to-main: methods that must not be moved into a uros chunk. Pin if:
	//   * Reads msg.sender — inside a chunk Txn.Sender = orch's app account,
	//     not the user; every auth check / balances lookup silently misbehaves.
	//   * Reads address(this) expecting main's address — chunks see __storage.
	// Manual list; no auto-detection.
	std::vector<std::string> pinnedToMain;

	// --fn-split <Name>:<idx>,...:g<N>[:cross]
	// Slice subroutine body into N+1 pieces at statement indices.
	// Without :cross, pieces share the same txn frame (scratch 100).
	// With :cross, pieces live on separate uros chunks and pass state
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
	// --deploy-pure-helpers. N indices → N+1 sidecar Contracts (e.g. 14KB →
	// two 7KB, fitting AVM 8KB cap). Live vars cross splits via scratch 100 +
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
