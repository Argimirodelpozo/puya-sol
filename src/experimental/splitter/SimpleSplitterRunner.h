#pragma once

#include "awst/Node.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace puyasol::splitter
{

/// Drives the SimpleSplitter pipeline end-to-end. Reads `--split-config`,
/// applies `--force-delegate`, calls SimpleSplitter::split per helper, and
/// writes each helper/orchestrator to its own subdirectory under outputDir
/// with awst.json, options.json, and (if puyaPath set) puya invocation.
///
class SimpleSplitterRunner
{
public:
	struct Config
	{
		/// Path to `--split-config <json>`. Empty string = no config file
		/// (no extraction unless `forceDelegate` is non-empty).
		std::string splitConfigPath;
		/// Externally routable method names that should each become their own
		/// state-preserving code page (from `--force-delegate <list>`).
		std::vector<std::string> forceDelegate;
		/// Per-method ensure_budget targets — threaded through to
		/// `SimpleSplitter::split`'s helper-method-body injection.
		std::map<std::string, uint64_t> ensureBudget;

		/// Output base directory + puya invocation parameters used for the
		/// per-helper output stage.
		std::string outputDir;
		std::string puyaPath;
		std::string logLevel;
		int optimizationLevel = 2;
		bool outputIr = false;
		bool noPuya = false;
		bool outputAsmReport = false;

		/// Source file path — used to disambiguate which root is the
		/// orchestrator (prefer the contract whose name matches the source
		/// stem).
		std::string sourceFile;
	};

	struct Result
	{
		/// True if any helper was extracted; main.cpp should skip its
		/// single-contract output path when true.
		bool didSplit = false;
		/// First non-zero puya exit code across all per-helper invocations.
		int puyaExitCode = 0;
	};

	/// Run the pipeline. `roots` is read but not modified.
	Result run(
		Config const& _cfg,
		std::vector<std::shared_ptr<awst::RootNode>> const& _roots);
};

} // namespace puyasol::splitter
