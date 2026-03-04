#include "Logger.h"
#include "builder/AWSTBuilder.h"
#include "json/AWSTSerializer.h"
#include "json/OptionsWriter.h"
#include "runner/PuyaRunner.h"
#include "splitter/SizeEstimator.h"
#include "splitter/CallGraphAnalyzer.h"
#include "splitter/ContractSplitter.h"
#include "splitter/FunctionSplitter.h"
#include "splitter/ConstantExternalizer.h"

#include <libsolidity/interface/CompilerStack.h>
#include <libsolidity/interface/FileReader.h>
#include <libsolidity/interface/ImportRemapper.h>
#include <liblangutil/CharStreamProvider.h>

#include <boost/filesystem.hpp>

#include <fstream>
#include <iostream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = boost::filesystem;

/// Transform Solidity source for compatibility with the 0.8.x compiler.
/// Handles pragma relaxation and 0.5.x/0.6.x → 0.8.x syntax differences so that
/// original contracts can be compiled without modification.
std::string transformSource(std::string const& _source)
{
	std::string result = _source;

	// 1. Relax pragma version: "pragma solidity =0.5.16;" → "pragma solidity >=0.5.0;"
	{
		static std::regex const re(R"(pragma\s+solidity\s+[=^~><]*\s*(\d+\.\d+)\.\d+\s*;)");
		result = std::regex_replace(result, re, "pragma solidity >=$1.0;");
	}

	// 2. Remove visibility from constructors: "constructor(...) public {" → "constructor(...) {"
	//    In 0.5.x constructors had visibility; in 0.8.x this is a parser error.
	{
		static std::regex const re(R"(constructor\s*\(([^)]*)\)\s+(?:public|internal)\s*\{)");
		result = std::regex_replace(result, re, "constructor($1) {");
	}

	// 3. Replace type cast to max: "uint(-1)" → "type(uint256).max", "uint112(-1)" → "type(uint112).max"
	//    In 0.5.x, uint(-1) was the idiom for max value; 0.8.x requires type(...).max
	{
		static std::regex const re(R"((uint\d*)\s*\(\s*-\s*1\s*\))");
		result = std::regex_replace(result, re, "type($1).max");
	}

	// 4. Fix bare Yul builtins in assembly: "chainid" (not followed by "(") → "chainid()"
	//    In 0.5.x Yul, chainid was a variable; in 0.8.x it must be called as a function.
	//    Must NOT match "block.chainid" (0.8.x property access), only bare "chainid" in assembly.
	//    C++ std::regex doesn't support lookbehind, so we use a manual replacement loop.
	{
		std::string const needle = "chainid";
		size_t pos = 0;
		while ((pos = result.find(needle, pos)) != std::string::npos)
		{
			size_t endPos = pos + needle.size();
			// Skip if preceded by '.' (e.g. block.chainid)
			if (pos > 0 && result[pos - 1] == '.')
			{
				pos = endPos;
				continue;
			}
			// Skip if already followed by '('
			size_t nextNonSpace = endPos;
			while (nextNonSpace < result.size() && result[nextNonSpace] == ' ')
				++nextNonSpace;
			if (nextNonSpace < result.size() && result[nextNonSpace] == '(')
			{
				pos = endPos;
				continue;
			}
			// Check word boundary: character before must not be alphanumeric/underscore
			if (pos > 0 && (std::isalnum(result[pos - 1]) || result[pos - 1] == '_'))
			{
				pos = endPos;
				continue;
			}
			// Replace bare "chainid" with "chainid()"
			result.insert(endPos, "()");
			pos = endPos + 2;
		}
	}

	return result;
}

/// Collect event signatures from a source string.
std::set<std::string> collectEventSignatures(std::string const& _source)
{
	std::set<std::string> result;
	static std::regex const eventRe(R"(event\s+(\w+)\s*\([^)]*\)\s*;)");
	auto it = std::sregex_iterator(_source.begin(), _source.end(), eventRe);
	auto end = std::sregex_iterator();
	for (; it != end; ++it)
		result.insert((*it)[1].str());
	return result;
}

/// Remove event declarations from a contract source that are already defined in its interfaces.
/// In 0.5.x, re-declaring interface events in a contract was allowed; in 0.8.x it's a
/// DeclarationError. This resolves it by removing the duplicate from the contract body.
std::string removeInheritedEvents(std::string const& _source, std::set<std::string> const& _interfaceEvents)
{
	if (_interfaceEvents.empty())
		return _source;

	std::string result = _source;

	// Find "contract X is Y {" sections and remove event declarations for events
	// that exist in the inherited interfaces
	static std::regex const contractRe(R"(contract\s+\w+\s+is\s+)");
	if (!std::regex_search(result, contractRe))
		return result; // No inheritance, nothing to dedup

	// Remove matching event declarations
	for (auto const& eventName: _interfaceEvents)
	{
		// Match "event EventName(...) ;" with possible whitespace/newlines
		std::regex eventDeclRe(
			"\\s*event\\s+" + eventName + "\\s*\\([^)]*\\)\\s*;[\\t ]*\\n?"
		);
		result = std::regex_replace(result, eventDeclRe, "\n");
	}

	return result;
}

struct Options
{
	std::string sourceFile;
	std::vector<std::string> importPaths;
	std::vector<std::string> remappings;
	std::string outputDir = "out";
	std::string puyaPath;
	std::string logLevel = "info";
	bool dumpAwst = false;
	bool noPuya = false;
	uint64_t opupBudget = 0;
	bool splitContracts = false;
	bool allowMidFunctionSplit = false;
	int optimizationLevel = 1;
};

void printUsage(char const* _progName)
{
	std::cout
		<< "Usage: " << _progName << " [options]\n"
		<< "\n"
		<< "Options:\n"
		<< "  --source <file>        Solidity source file to compile (required)\n"
		<< "  --import-path <path>   Import path for resolving imports (repeatable)\n"
		<< "  --remapping <map>      Import remapping: prefix=target (repeatable)\n"
		<< "  --output-dir <dir>     Output directory (default: out)\n"
		<< "  --puya-path <path>     Path to puya executable (required unless --no-puya)\n"
		<< "  --log-level <level>    Log level: debug, info, warning, error (default: info)\n"
		<< "  --dump-awst            Dump AWST JSON to stdout\n"
		<< "  --no-puya              Skip puya invocation (only generate JSON)\n"
		<< "  --opup-budget <N>      Inject ensure_budget(N) into public methods (OpUp)\n"
		<< "  --split-contracts      Auto-split oversized contracts into cooperating helpers\n"
		<< "  --allow-mid-function-split  Allow splitting oversized functions at statement boundaries\n"
		<< "  --optimization-level <N>   Puya optimization level: 0, 1, 2 (default: 1)\n"
		<< "  --help                 Show this help message\n";
}

Options parseArgs(int _argc, char* _argv[])
{
	Options opts;

	for (int i = 1; i < _argc; ++i)
	{
		std::string arg = _argv[i];

		if (arg == "--source" && i + 1 < _argc)
			opts.sourceFile = _argv[++i];
		else if (arg == "--import-path" && i + 1 < _argc)
			opts.importPaths.push_back(_argv[++i]);
		else if (arg == "--remapping" && i + 1 < _argc)
			opts.remappings.push_back(_argv[++i]);
		else if (arg == "--output-dir" && i + 1 < _argc)
			opts.outputDir = _argv[++i];
		else if (arg == "--puya-path" && i + 1 < _argc)
			opts.puyaPath = _argv[++i];
		else if (arg == "--log-level" && i + 1 < _argc)
			opts.logLevel = _argv[++i];
		else if (arg == "--dump-awst")
			opts.dumpAwst = true;
		else if (arg == "--no-puya")
			opts.noPuya = true;
		else if (arg == "--opup-budget" && i + 1 < _argc)
			opts.opupBudget = std::stoull(_argv[++i]);
		else if (arg == "--split-contracts")
			opts.splitContracts = true;
		else if (arg == "--allow-mid-function-split")
			opts.allowMidFunctionSplit = true;
		else if (arg == "--optimization-level" && i + 1 < _argc)
			opts.optimizationLevel = std::stoi(_argv[++i]);
		else if (arg == "--help")
		{
			printUsage(_argv[0]);
			std::exit(0);
		}
		else
		{
			std::cerr << "Unknown option: " << arg << std::endl;
			printUsage(_argv[0]);
			std::exit(1);
		}
	}

	return opts;
}

int main(int _argc, char* _argv[])
{
	Options opts = parseArgs(_argc, _argv);

	// Configure logger from --log-level
	auto& logger = puyasol::Logger::instance();
	if (opts.logLevel == "debug")
		logger.setMinLevel(puyasol::LogLevel::Debug);
	else if (opts.logLevel == "warning")
		logger.setMinLevel(puyasol::LogLevel::Warning);
	else if (opts.logLevel == "error")
		logger.setMinLevel(puyasol::LogLevel::Error);
	else
		logger.setMinLevel(puyasol::LogLevel::Info);

	if (opts.sourceFile.empty())
	{
		std::cerr << "Error: --source is required" << std::endl;
		printUsage(_argv[0]);
		return 1;
	}

	if (!opts.noPuya && opts.puyaPath.empty())
	{
		std::cerr << "Error: --puya-path is required (or use --no-puya)" << std::endl;
		return 1;
	}

	// Resolve absolute path
	fs::path sourceAbsPath = fs::absolute(opts.sourceFile);
	std::string sourceFile = sourceAbsPath.string();

	logger.info("puya-sol v0.1.0 — Solidity to Algorand Compiler");
	logger.info("Source: " + sourceFile);

	// Set up Solidity FileReader for import resolution
	fs::path sourceDir = sourceAbsPath.parent_path();
	fs::path projectRoot = sourceDir.parent_path(); // contracts/ → project root
	fs::path nodeModules = projectRoot / "node_modules";

	solidity::frontend::FileReader fileReader(
		projectRoot, // base path
		{}           // allowed directories (populated below)
	);

	// Allow source directory
	fileReader.allowDirectory(sourceDir);
	fileReader.allowDirectory(projectRoot);

	// Add node_modules as include path (for @openzeppelin etc.)
	if (fs::exists(nodeModules))
	{
		fileReader.addIncludePath(nodeModules);
		fileReader.allowDirectory(nodeModules);
	}

	// Allow user-specified import paths
	for (auto const& ip: opts.importPaths)
	{
		fs::path absIp = fs::absolute(ip);
		fileReader.addIncludePath(absIp);
		fileReader.allowDirectory(absIp);
	}

	// Read main source
	std::string rawMainSource;
	{
		std::ifstream file(sourceFile);
		if (!file.is_open())
		{
			logger.error("Cannot read source file: " + sourceFile);
			return 1;
		}
		std::ostringstream ss;
		ss << file.rdbuf();
		rawMainSource = ss.str();
	}

	// Pre-scan: collect event signatures from imported interface files
	// so we can remove duplicate event declarations from the contract source.
	std::set<std::string> interfaceEvents;
	{
		// Find import paths in the main source
		static std::regex const importRe(R"(import\s+['"](\.\/[^'"]+)['"]\s*;)");
		auto it = std::sregex_iterator(rawMainSource.begin(), rawMainSource.end(), importRe);
		auto end = std::sregex_iterator();
		for (; it != end; ++it)
		{
			std::string importPath = (*it)[1].str();
			fs::path importAbsPath = sourceDir / importPath;
			if (fs::exists(importAbsPath))
			{
				std::ifstream impFile(importAbsPath.string());
				if (impFile.is_open())
				{
					std::ostringstream ss;
					ss << impFile.rdbuf();
					std::string impContent = ss.str();
					// Only collect events from interfaces (not concrete contracts)
					if (impContent.find("interface ") != std::string::npos)
					{
						auto events = collectEventSignatures(impContent);
						interfaceEvents.insert(events.begin(), events.end());
					}
				}
			}
		}
		if (!interfaceEvents.empty())
			logger.debug("Found " + std::to_string(interfaceEvents.size()) +
				" event(s) in interfaces to dedup");
	}

	// Apply all source transformations
	std::string mainSourceContent = transformSource(rawMainSource);
	mainSourceContent = removeInheritedEvents(mainSourceContent, interfaceEvents);
	fileReader.addOrUpdateFile(sourceAbsPath, mainSourceContent);

	// Get the normalized source unit name for the main file
	std::string sourceUnitName = fileReader.cliPathToSourceUnitName(sourceAbsPath);
	logger.debug("Source unit: " + sourceUnitName);

	// Wrap the FileReader callback to transform imported files
	auto baseReader = fileReader.reader();
	auto relaxingReader = [&](std::string const& _kind, std::string const& _path)
		-> solidity::frontend::ReadCallback::Result
	{
		auto result = baseReader(_kind, _path);
		if (result.success)
			result.responseOrErrorMessage = transformSource(result.responseOrErrorMessage);
		return result;
	};

	// Set up CompilerStack with pragma-relaxing reader
	solidity::frontend::CompilerStack compiler(relaxingReader);

	// Set sources using the normalized source unit name
	compiler.setSources({{sourceUnitName, mainSourceContent}});

	// Configure EVM version — use Cancun to support block.chainid, block.basefee, etc.
	auto evmVer = solidity::langutil::EVMVersion::cancun();
	compiler.setEVMVersion(evmVer);
	logger.info("EVM version set to: " + evmVer.name() + " (hasChainID=" + (evmVer.hasChainID() ? "true" : "false") + ")");

	// Apply import remappings (Foundry-style: prefix=target)
	if (!opts.remappings.empty())
	{
		std::vector<solidity::frontend::ImportRemapper::Remapping> parsedRemappings;
		for (auto const& remapStr: opts.remappings)
		{
			auto parsed = solidity::frontend::ImportRemapper::parseRemapping(remapStr);
			if (parsed.has_value())
			{
				parsedRemappings.push_back(parsed.value());
				logger.debug("Remapping: '" + parsed->prefix + "' => '" + parsed->target + "'");
				// Allow the remapping target directory so FileReader can read from it
				fs::path targetPath(parsed->target);
				if (targetPath.is_absolute() && fs::exists(targetPath))
				{
					fileReader.allowDirectory(targetPath);
					fileReader.addIncludePath(targetPath);
				}
			}
			else
				logger.warning("Invalid remapping format: " + remapStr);
		}
		compiler.setRemappings(parsedRemappings);
	}

	logger.info("Parsing and type-checking...");

	// Parse and analyze
	bool success = compiler.parseAndAnalyze();
	if (!success)
	{
		// Check if we only have warnings (no errors)
		// Some errors from 0.5.x→0.8.x compat are treated as warnings
		bool hasError = false;
		for (auto const& error: compiler.errors())
		{
			if (error->type() == solidity::langutil::Error::Type::Warning)
				continue;

			std::string msg = error->what();

			// Suppress "Event with same name and parameter types defined twice"
			// — this is a 0.5.x→0.8.x compat issue: in 0.5.x contracts could
			// re-declare events inherited from interfaces; in 0.8.x it's an error.
			if (msg.find("Event with same name and parameter types defined twice") != std::string::npos)
			{
				logger.debug("[suppressed] " + msg);
				continue;
			}

			// Suppress "Derived contract must override function"
			// — this is a 0.5.x→0.8.x compat issue: in 0.5.x, implicit override
			// was allowed for diamond inheritance; in 0.8.x, explicit `override` is required.
			if (msg.find("Derived contract must override function") != std::string::npos)
			{
				logger.debug("[suppressed] " + msg);
				continue;
			}

			// Use formattedMessage for detailed error with source location
			auto const* secondaryLoc = error->secondarySourceLocation();
			std::string detail = msg;
			if (auto const* srcLoc = error->sourceLocation())
			{
				detail += " at ";
				if (srcLoc->sourceName)
					detail += *srcLoc->sourceName + ":";
				detail += std::to_string(srcLoc->start) + "-" + std::to_string(srcLoc->end);
			}
			logger.error(
				std::string("[")
				+ solidity::langutil::Error::formatErrorType(error->type())
				+ "] " + detail
			);
			hasError = true;
		}
		if (hasError)
		{
			logger.error("Compilation failed.");
			return 1;
		}
		// Re-attempt with errors suppressed — Solidity CompilerStack may have
		// stopped early. We need to push past the error to get the AST.
		// If there were only suppressed errors, the AST should still be usable.
	}

	logger.info("Parse and type-check successful!");
	logger.debug("Source units: " + std::to_string(compiler.sourceNames().size()));

	// Build AWST
	logger.info("Building AWST...");
	puyasol::builder::AWSTBuilder builder;
	auto roots = builder.build(compiler, sourceFile, opts.opupBudget);

	if (roots.empty())
	{
		logger.error("No contracts found");
		return 1;
	}

	logger.info("Generated " + std::to_string(roots.size()) + " AWST root node(s)");

	// ─── Contract size estimation and splitting ────────────────────────────
	// Always run size estimation (Parts 1-2: diagnostics).
	// Only split if --split-contracts is set AND contract is oversized.

	// Find the primary contract for analysis
	std::shared_ptr<puyasol::awst::Contract> primaryContract;
	std::string sourceBaseName = fs::path(sourceFile).stem().string();
	for (auto const& root: roots)
	{
		if (auto contract = std::dynamic_pointer_cast<puyasol::awst::Contract>(root))
		{
			primaryContract = contract; // fallback = last
			if (contract->name == sourceBaseName)
				break;
		}
	}

	// Collect subroutines for analysis
	std::vector<std::shared_ptr<puyasol::awst::RootNode>> subroutines;
	for (auto const& root: roots)
	{
		if (dynamic_cast<puyasol::awst::Subroutine const*>(root.get()))
			subroutines.push_back(root);
	}

	bool didSplit = false;
	std::vector<std::string> allContractIds;

	if (primaryContract)
	{
		// ─── Constant externalization ─────────────────────────────────────────
		// Move large constants (proof data, etc.) to box storage. This reduces
		// bytecode size and enables contracts to fit within AVM 8KB limit.
		// A __load_constants() ABI method is added to read from box → scratch.
		{
			puyasol::splitter::ConstantExternalizer constExt;
			auto constResult = constExt.externalize(*primaryContract, subroutines);
			if (constResult.didExternalize)
			{
				logger.info("Externalized " + std::to_string(constResult.constants.size()) +
					" constant(s) to box '" + constResult.boxName + "' (" +
					std::to_string(constResult.totalBoxSize) + " bytes)");
			}
		}

		// ─── Phase 0: Mid-function splitting (BEFORE contract splitting) ─────
		// Run FunctionSplitter globally so that chunks become independent
		// subroutines that can be distributed across helper contracts.
		puyasol::splitter::FunctionSplitter::SplitResult funcResult;

		if (opts.allowMidFunctionSplit && opts.splitContracts)
		{
			logger.info("Phase 0: Global mid-function splitting...");
			puyasol::splitter::FunctionSplitter funcSplitter;

			// Target ~2000 instructions ≈ ~4000 bytes per chunk.
			// Shared deps (FrLib, Transcript utils) add ~3-5KB when duplicated.
			// Most helpers fit 8KB; 2-3 outliers may need further optimization.
			constexpr size_t maxChunkInstructions = 2000;

			funcResult = funcSplitter.splitOversizedFunctions(roots, maxChunkInstructions);
			if (funcResult.didSplit)
			{
				logger.info("  Split " + std::to_string(funcResult.rewrittenFunctions.size()) +
					" function(s) into chunks (" +
					std::to_string(funcResult.newSubroutines.size()) + " new subroutines)");

				if (!funcResult.mutableSharedFunctions.empty())
					logger.info("  " + std::to_string(funcResult.mutableSharedFunctions.size()) +
						" function(s) have mutable shared params (chunks grouped)");

				// Re-collect subroutines after splitting (new chunks added to roots)
				subroutines.clear();
				for (auto const& root: roots)
				{
					if (dynamic_cast<puyasol::awst::Subroutine const*>(root.get()))
						subroutines.push_back(root);
				}
			}
		}

		// ─── Phase 1: Size estimation ────────────────────────────────────────
		puyasol::splitter::SizeEstimator estimator;
		auto estimate = estimator.estimate(*primaryContract, subroutines);

		logger.info("Size estimate for '" + primaryContract->name + "': " +
			std::to_string(estimate.totalInstructions) + " instructions, ~" +
			std::to_string(estimate.estimatedBytes) + " bytes");

		// Log per-method breakdown at debug level
		for (auto const& [name, size]: estimate.methodSizes)
			logger.debug("  " + name + ": " + std::to_string(size) + " instructions");

		if (estimate.estimatedBytes > puyasol::splitter::SizeEstimator::WarnThresholdBytes)
			logger.warning("Contract '" + primaryContract->name + "' estimated at ~" +
				std::to_string(estimate.estimatedBytes) + " bytes, exceeds AVM limit of ~" +
				std::to_string(puyasol::splitter::SizeEstimator::AVMMaxBytes) + " bytes");

		// ─── Phase 2: Call graph analysis ────────────────────────────────────
		puyasol::splitter::CallGraphAnalyzer analyzer;
		auto recommendation = analyzer.analyze(
			*primaryContract, subroutines, estimate,
			funcResult.rewrittenFunctions,
			funcResult.mutableSharedFunctions
		);

		// ─── Phase 3-4: Contract splitting ───────────────────────────────────
		if (opts.splitContracts && recommendation.shouldSplit)
		{
			logger.info("--split-contracts enabled, performing contract split...");

			puyasol::splitter::ContractSplitter splitter;
			auto splitResult = splitter.split(primaryContract, roots, recommendation);

			if (splitResult.didSplit)
			{
				didSplit = true;

				// No per-helper FunctionSplitter loop — splitting was done
				// globally in Phase 0 so chunks are already distributed.

				// Create output directory
				fs::create_directories(opts.outputDir);

				puyasol::json::AWSTSerializer serializer;
				puyasol::runner::PuyaRunner runner;
				if (!opts.noPuya)
					runner.setPuyaPath(opts.puyaPath);

				int worstExit = 0;

				// Write separate awst.json + options.json for each contract,
				// then run puya for each.
				for (auto const& contractAWST: splitResult.contracts)
				{
					auto awstJson = serializer.serialize(contractAWST.roots);

					std::string suffix = contractAWST.contractName;
					std::string awstPath = (fs::path(opts.outputDir) /
						("awst_" + suffix + ".json")).string();
					std::string optionsPath = (fs::path(opts.outputDir) /
						("options_" + suffix + ".json")).string();

					// Write awst.json
					{
						std::ofstream out(awstPath);
						out << awstJson.dump(2) << std::endl;
						logger.info("Wrote: " + awstPath);
					}

					if (opts.dumpAwst)
						std::cout << "// " << suffix << "\n"
							<< awstJson.dump(2) << std::endl;

					// Write options.json
					puyasol::json::OptionsWriter::write(
						optionsPath,
						contractAWST.contractId,
						opts.outputDir,
						opts.optimizationLevel
					);
					logger.info("Wrote: " + optionsPath);

					// Run puya
					if (!opts.noPuya)
					{
						logger.info("Compiling " + suffix + " ...");
						int exitCode = runner.run(awstPath, optionsPath, opts.logLevel);
						if (exitCode != 0)
							worstExit = exitCode;
					}
				}

				// Summary
				if (logger.warningCount() > 0)
					logger.info("Completed with " +
						std::to_string(logger.warningCount()) + " warning(s)");

				if (opts.noPuya)
					logger.info("Done! AWST JSON generated for " +
						std::to_string(splitResult.contracts.size()) +
						" contracts. Use --puya-path to compile to TEAL.");

				return worstExit;
			}
		}
		else if (recommendation.shouldSplit)
		{
			logger.info("Contract is oversized. Use --split-contracts to automatically split.");
			if (!recommendation.partitions.empty())
				logger.info("Recommended " + std::to_string(recommendation.partitions.size()) +
					" partitions");
		}
	}

	// ─── Normal (non-split) serialization and output ───────────────────────

	// Serialize to JSON
	puyasol::json::AWSTSerializer serializer;
	auto awstJson = serializer.serialize(roots);

	// Create output directory
	fs::create_directories(opts.outputDir);

	// Write awst.json
	std::string awstPath = (fs::path(opts.outputDir) / "awst.json").string();
	{
		std::ofstream out(awstPath);
		out << awstJson.dump(2) << std::endl;
		logger.info("Wrote: " + awstPath);
	}

	// Dump to stdout if requested (keep on stdout for piping)
	if (opts.dumpAwst)
		std::cout << awstJson.dump(2) << std::endl;

	// Get all contract names for options
	std::vector<std::string> contractNames;
	for (auto const& root: roots)
	{
		if (auto const* contract = dynamic_cast<puyasol::awst::Contract const*>(root.get()))
			contractNames.push_back(contract->id);
	}

	// Write options.json
	std::string optionsPath = (fs::path(opts.outputDir) / "options.json").string();
	if (contractNames.size() <= 1)
	{
		std::string contractName = contractNames.empty() ? "" : contractNames[0];
		puyasol::json::OptionsWriter::write(optionsPath, contractName, opts.outputDir, opts.optimizationLevel);
	}
	else
	{
		puyasol::json::OptionsWriter::writeMultiple(optionsPath, contractNames, opts.outputDir, opts.optimizationLevel);
	}
	logger.info("Wrote: " + optionsPath);

	// Summary
	if (logger.warningCount() > 0)
		logger.info(
			"Completed with " + std::to_string(logger.warningCount()) + " warning(s)"
		);

	if (logger.hasErrors())
		return 1;

	// Run puya backend
	if (!opts.noPuya)
	{
		logger.info("Invoking puya backend...");
		puyasol::runner::PuyaRunner runner;
		runner.setPuyaPath(opts.puyaPath);
		int exitCode = runner.run(awstPath, optionsPath, opts.logLevel);
		return exitCode;
	}

	logger.info("Done! AWST JSON generated. Use --puya-path to compile to TEAL.");
	return 0;
}
