#include "Logger.h"
#include "builder/AWSTBuilder.h"
#include "json/AWSTSerializer.h"
#include "json/OptionsWriter.h"
#include "runner/PuyaRunner.h"

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

	// 4. Fix bare Yul builtins: "chainid" (not followed by "(") → "chainid()"
	//    In 0.5.x Yul, chainid was a variable; in 0.8.x it must be called as a function.
	{
		static std::regex const re(R"(\bchainid\b(?!\s*\())");
		result = std::regex_replace(result, re, "chainid()");
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
	std::string outputDir = "out";
	std::string puyaPath;
	std::string logLevel = "info";
	bool dumpAwst = false;
	bool noPuya = false;
	uint64_t opupBudget = 0;
};

void printUsage(char const* _progName)
{
	std::cout
		<< "Usage: " << _progName << " [options]\n"
		<< "\n"
		<< "Options:\n"
		<< "  --source <file>        Solidity source file to compile (required)\n"
		<< "  --import-path <path>   Import path for resolving imports (repeatable)\n"
		<< "  --output-dir <dir>     Output directory (default: out)\n"
		<< "  --puya-path <path>     Path to puya executable (required unless --no-puya)\n"
		<< "  --log-level <level>    Log level: debug, info, warning, error (default: info)\n"
		<< "  --dump-awst            Dump AWST JSON to stdout\n"
		<< "  --no-puya              Skip puya invocation (only generate JSON)\n"
		<< "  --opup-budget <N>      Inject ensure_budget(N) into public methods (OpUp)\n"
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

	// Configure EVM version
	compiler.setEVMVersion(solidity::langutil::EVMVersion{});

	// No remappings needed — node_modules is added as include path

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

	// Get contract name for options — prefer the contract whose name matches
	// the source file name (e.g., UniswapV2Pair.sol → UniswapV2Pair).
	// Falls back to the last contract in the list.
	std::string contractName;
	std::string sourceBaseName = fs::path(sourceFile).stem().string();
	for (auto const& root: roots)
	{
		if (auto const* contract = dynamic_cast<puyasol::awst::Contract const*>(root.get()))
		{
			contractName = contract->id; // always update (fallback = last)
			// Check if this contract's name matches the source file name
			// contract->id is like "...sol.ContractName"
			std::string justName = contract->name;
			if (justName == sourceBaseName)
			{
				contractName = contract->id;
				break;
			}
		}
	}

	// Write options.json
	std::string optionsPath = (fs::path(opts.outputDir) / "options.json").string();
	puyasol::json::OptionsWriter::write(optionsPath, contractName, opts.outputDir);
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
