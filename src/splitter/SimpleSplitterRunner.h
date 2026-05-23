#pragma once

#include "awst/Node.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace puyasol::splitter
{

/// Drives the SimpleSplitter pipeline end-to-end so main.cpp doesn't have
/// to know any of its internals. Reads a `--split-config <json>` file,
/// applies `--force-delegate` extras, calls `SimpleSplitter::split` per
/// helper, and (when the pipeline produced any split contracts) writes
/// each helper / orchestrator to its own subdirectory under
/// `outputDir` with the per-contract `awst.json`, `options.json`, and
/// (if `puyaPath` is set) per-contract puya invocation.
///
/// SimpleSplitter is a separate pipeline from UrosSplitter — caller is
/// responsible for ensuring the two aren't both configured for the same
/// invocation.
class SimpleSplitterRunner
{
public:
	struct Config
	{
		/// Path to `--split-config <json>`. Empty string = no config file
		/// (no extraction unless `forceDelegate` is non-empty).
		std::string splitConfigPath;
		/// Function names that should each become their own one-method
		/// helper extraction (from `--force-delegate <list>`).
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
		/// True if any helper was extracted (i.e. the runner took over
		/// output and main.cpp should skip its single-contract output
		/// path). False if no split happened — main.cpp continues normally.
		bool didSplit = false;
		/// Aggregate puya exit code across all per-helper invocations.
		/// 0 if every helper succeeded, otherwise the first non-zero
		/// exit code.
		int puyaExitCode = 0;
	};

	/// Run the pipeline. `roots` is in/out — when the splitter runs,
	/// `roots` is left unchanged (the runner reads it to construct
	/// per-helper subdirs but doesn't need to feed the remainder back
	/// since it owns the output).
	Result run(
		Config const& _cfg,
		std::vector<std::shared_ptr<awst::RootNode>> const& _roots);
};

} // namespace puyasol::splitter
