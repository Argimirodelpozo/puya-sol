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
#include <sstream>
#include <string>
#include <vector>

namespace fs = boost::filesystem;

/// Replace exact pragma version (e.g. "pragma solidity 0.8.11;") with a range
/// so that the develop compiler (which is considered < the released version) can parse it.
std::string relaxPragma(std::string const& _source)
{
	// Match "pragma solidity X.Y.Z;" and replace with "pragma solidity >=X.Y.0;"
	static std::regex const re(R"(pragma\s+solidity\s+(\d+\.\d+)\.\d+\s*;)");
	return std::regex_replace(_source, re, "pragma solidity >=$1.0;");
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

	// Read main source (relax pragma for develop compiler)
	std::string mainSourceContent;
	{
		std::ifstream file(sourceFile);
		if (!file.is_open())
		{
			logger.error("Cannot read source file: " + sourceFile);
			return 1;
		}
		std::ostringstream ss;
		ss << file.rdbuf();
		mainSourceContent = relaxPragma(ss.str());
		fileReader.addOrUpdateFile(sourceAbsPath, mainSourceContent);
	}

	// Get the normalized source unit name for the main file
	std::string sourceUnitName = fileReader.cliPathToSourceUnitName(sourceAbsPath);
	logger.debug("Source unit: " + sourceUnitName);

	// Wrap the FileReader callback to relax pragmas on imported files
	auto baseReader = fileReader.reader();
	auto relaxingReader = [&](std::string const& _kind, std::string const& _path)
		-> solidity::frontend::ReadCallback::Result
	{
		auto result = baseReader(_kind, _path);
		if (result.success)
			result.responseOrErrorMessage = relaxPragma(result.responseOrErrorMessage);
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
		bool hasError = false;
		for (auto const& error: compiler.errors())
		{
			if (error->type() != solidity::langutil::Error::Type::Warning)
			{
				logger.error(
					std::string("[")
					+ solidity::langutil::Error::formatErrorType(error->type())
					+ "] " + error->what()
				);
				hasError = true;
			}
		}
		if (hasError)
		{
			logger.error("Compilation failed.");
			return 1;
		}
	}

	logger.info("Parse and type-check successful!");
	logger.debug("Source units: " + std::to_string(compiler.sourceNames().size()));

	// Build AWST
	logger.info("Building AWST...");
	puyasol::builder::AWSTBuilder builder;
	auto roots = builder.build(compiler, sourceFile);

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

	// Get contract name for options
	std::string contractName;
	for (auto const& root: roots)
	{
		if (auto const* contract = dynamic_cast<puyasol::awst::Contract const*>(root.get()))
		{
			contractName = contract->id;
			break;
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
