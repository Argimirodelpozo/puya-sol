#include "Logger.h"
#include "builder/AWSTBuilder.h"
#include "builder/assembly/AssemblyBuilder.h"
#include "builder/sol-ast/calls/SolNewExpression.h"
#include "json/AWSTSerializer.h"
#include "json/OptionsWriter.h"
#include "runner/PuyaRunner.h"
#include "splitter/UrosSplitter.h"

#include <libsolidity/interface/CompilerStack.h>
#include <libsolidity/interface/FileReader.h>
#include <libsolidity/interface/ImportRemapper.h>
#include <liblangutil/CharStreamProvider.h>

#include <boost/filesystem.hpp>

#include <unistd.h>

#include <fstream>
#include <nlohmann/json.hpp>
using njson = nlohmann::ordered_json;
#include <iostream>
#include <optional>
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
	int optimizationLevel = 1;
	bool outputIr = false;
	bool outputLogs = true;
	bool viaYulBehavior = false;
	std::string evmVersion;     // empty = compiler default (cancun)
	// Each --uros-splitter flag invocation is one chunk's method list.
	// urosSplitGroups[i] is the methods that go into chunk i.
	std::vector<std::vector<std::string>> urosSplitGroups;
	int64_t urosOrchAppId = 0; // orchestrator app id baked into stub guards
};

void printUsage(char const* _progName)
{
	std::cout
		<< "Usage: " << _progName << " [options]\n"
		<< "\n"
		<< "Options:\n"
		<< "  --source <file>        Solidity source file (required, repeatable for multi-file)\n"
		<< "  --import-path <path>   Import path for resolving imports (repeatable)\n"
		<< "  --remapping <map>      Import remapping: prefix=target (repeatable)\n"
		<< "  --output-dir <dir>     Output directory (default: out)\n"
		<< "  --puya-path <path>     Path to puya executable (required unless --no-puya)\n"
		<< "  --log-level <level>    Log level: debug, info, warning, error (default: info)\n"
		<< "  --dump-awst            Dump AWST JSON to stdout\n"
		<< "  --no-puya              Skip puya invocation (only generate JSON)\n"
		<< "  --opup-budget <N>      Inject ensure_budget(N) into ALL public methods (OpUp)\n"
		<< "  --ensure-budget <f:N>  Inject ensure_budget(N) into function f (repeatable)\n"
		<< "  --optimization-level <N>   Puya optimization level: 0, 1, 2 (default: 1)\n"
		<< "  --output-ir            Output all intermediate representations (SSA IR, MIR, TEAL)\n"
		<< "  --no-output-logs       Disable writing compilation logs to output directory\n"
		<< "  --via-yul-behavior     Emulate Solidity's viaIR/compileViaYul codegen semantics\n"
		<< "                         (separate subroutines per modifier, fresh vars per _ invocation)\n"
		<< "  --evm-version <name>   EVM version for the Solidity parser. Accepts the same\n"
		<< "                         names solc supports: homestead..osaka. Default: cancun.\n"
		<< "  --uros-splitter <list> Comma-separated method names to split out into a sidecar\n"
		<< "                         contract. Main keeps stubs; <Name>__split.approval.bin\n"
		<< "                         is emitted alongside <Name>.approval.bin. Run-time swap\n"
		<< "                         is performed by a separate orchestrator app (see\n"
		<< "                         src/splitter/uros_orchestrator.py).\n"
		<< "  --uros-orch-app-id <N> Substitute TMPL_UROS_ORCH_APP_ID with N at compile time.\n"
		<< "                         Required for splitter stubs' next-txn-is-orch.dispatch()\n"
		<< "                         guard to admit calls. Typically set on a SECOND compile\n"
		<< "                         pass after the orchestrator is deployed and its app id\n"
		<< "                         is known.\n"
		<< "  --help                 Show this help message\n";
}

Options parseArgs(int _argc, char* _argv[])
{
	Options opts;

	for (int i = 1; i < _argc; ++i)
	{
		std::string arg = _argv[i];

		if (arg == "--source" && i + 1 < _argc)
			opts.sourceFiles.push_back(_argv[++i]);
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
		else if (arg == "--ensure-budget" && i + 1 < _argc)
		{
			// Format: func_name:budget (e.g., "f:20000")
			std::string spec = _argv[++i];
			auto colon = spec.find(':');
			if (colon != std::string::npos)
				opts.ensureBudget[spec.substr(0, colon)] = std::stoull(spec.substr(colon + 1));
		}
		else if (arg == "--optimization-level" && i + 1 < _argc)
			opts.optimizationLevel = std::stoi(_argv[++i]);
		else if (arg == "--output-ir")
			opts.outputIr = true;
		else if (arg == "--no-output-logs")
			opts.outputLogs = false;
		else if (arg == "--via-yul-behavior")
			opts.viaYulBehavior = true;
		else if (arg == "--evm-version" && i + 1 < _argc)
			opts.evmVersion = _argv[++i];
		else if (arg == "--uros-orch-app-id" && i + 1 < _argc)
			opts.urosOrchAppId = std::stoll(_argv[++i]);
		else if (arg == "--uros-splitter" && i + 1 < _argc)
		{
			// Comma-separated method names. The flag is repeatable —
			// each invocation defines one CHUNK's method list. e.g.
			//   --uros-splitter "fooA,fooB" --uros-splitter "fooC,fooD"
			// produces 2 chunks. Single invocation = 1 chunk.
			std::string spec = _argv[++i];
			std::vector<std::string> group;
			size_t start = 0;
			while (start <= spec.size())
			{
				size_t comma = spec.find(',', start);
				size_t end = (comma == std::string::npos) ? spec.size() : comma;
				std::string name = spec.substr(start, end - start);
				while (!name.empty() && std::isspace(static_cast<unsigned char>(name.front())))
					name.erase(name.begin());
				while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back())))
					name.pop_back();
				if (!name.empty())
					group.push_back(std::move(name));
				if (comma == std::string::npos) break;
				start = comma + 1;
			}
			if (!group.empty())
				opts.urosSplitGroups.push_back(std::move(group));
		}
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

/// Configure the global Logger from --log-level + --output-dir options.
static void configureLogger(Options const& _opts)
{
	auto& logger = puyasol::Logger::instance();
	if (_opts.logLevel == "debug")
		logger.setMinLevel(puyasol::LogLevel::Debug);
	else if (_opts.logLevel == "warning")
		logger.setMinLevel(puyasol::LogLevel::Warning);
	else if (_opts.logLevel == "error")
		logger.setMinLevel(puyasol::LogLevel::Error);
	else
		logger.setMinLevel(puyasol::LogLevel::Info);

	if (_opts.outputLogs)
	{
		fs::create_directories(_opts.outputDir);
		std::string logPath = (fs::path(_opts.outputDir) / "puya-sol.log").string();
		logger.setOutputLogFile(logPath);
	}
}

/// Set up the Solidity FileReader with allowed directories and include
/// paths: source dir, project root, optional node_modules, the puya-sol
/// stdlib at `<exe>/../WIP/`, and any user-specified --import-path entries.
static solidity::frontend::FileReader setupFileReader(
	Options const& _opts,
	fs::path const& _sourceDir,
	fs::path const& _projectRoot)
{
	fs::path nodeModules = _projectRoot / "node_modules";

	solidity::frontend::FileReader fileReader(
		_projectRoot, // base path
		{}            // allowed directories (populated below)
	);

	fileReader.allowDirectory(_sourceDir);
	fileReader.allowDirectory(_projectRoot);

	// Add node_modules as include path (for @openzeppelin etc.)
	if (fs::exists(nodeModules))
	{
		fileReader.addIncludePath(nodeModules);
		fileReader.allowDirectory(nodeModules);
	}

	// puya-sol stdlib: locate `WIP/tokens/` relative to the executable
	// (build/puya-sol → ../WIP/tokens/) so user contracts can `import
	// "tokens/AERC20.sol"` regardless of where they live in the tree.
	// Resolved via /proc/self/exe; falls back to a no-op on platforms
	// without procfs.
	try
	{
		char execPathBuf[4096];
		ssize_t len = ::readlink("/proc/self/exe", execPathBuf, sizeof(execPathBuf) - 1);
		if (len > 0)
		{
			execPathBuf[len] = '\0';
			fs::path stdlibBase = fs::path(execPathBuf).parent_path().parent_path() / "WIP";
			if (fs::exists(stdlibBase / "tokens"))
			{
				fileReader.addIncludePath(stdlibBase);
				fileReader.allowDirectory(stdlibBase);
			}
		}
	}
	catch (...) { /* best-effort, never fatal */ }

	// Allow user-specified import paths
	for (auto const& ip: _opts.importPaths)
	{
		fs::path absIp = fs::absolute(ip);
		fileReader.addIncludePath(absIp);
		fileReader.allowDirectory(absIp);
	}

	return fileReader;
}

/// Read `_path` into a string. Returns std::nullopt if the file can't be opened.
static std::optional<std::string> readSourceFile(std::string const& _path)
{
	std::ifstream file(_path);
	if (!file.is_open()) return std::nullopt;
	std::ostringstream ss;
	ss << file.rdbuf();
	return ss.str();
}

int main(int _argc, char* _argv[])
{
	Options opts = parseArgs(_argc, _argv);
	configureLogger(opts);
	auto& logger = puyasol::Logger::instance();

	if (opts.sourceFiles.empty())
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

	// Resolve absolute path (first source is the "main" source)
	fs::path sourceAbsPath = fs::absolute(opts.sourceFiles[0]);
	std::string sourceFile = sourceAbsPath.string();

	logger.info("puya-sol v0.1.0 — Solidity to Algorand Compiler");
	logger.info("Source: " + sourceFile);

	fs::path sourceDir = sourceAbsPath.parent_path();
	fs::path projectRoot = sourceDir.parent_path(); // contracts/ → project root
	auto fileReader = setupFileReader(opts, sourceDir, projectRoot);

	auto rawMainSourceOpt = readSourceFile(sourceFile);
	if (!rawMainSourceOpt)
	{
		logger.error("Cannot read source file: " + sourceFile);
		return 1;
	}
	std::string rawMainSource = std::move(*rawMainSourceOpt);

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

	// Set sources — main source + any additional source files
	std::map<std::string, std::string> sources;
	sources[sourceUnitName] = mainSourceContent;
	for (size_t i = 1; i < opts.sourceFiles.size(); ++i)
	{
		fs::path extraPath = fs::absolute(opts.sourceFiles[i]);
		std::ifstream extraFile(extraPath.string());
		if (extraFile)
		{
			std::string extraContent((std::istreambuf_iterator<char>(extraFile)),
				std::istreambuf_iterator<char>());
			extraContent = transformSource(extraContent);
			std::string extraUnit = fileReader.cliPathToSourceUnitName(extraPath);
			sources[extraUnit] = extraContent;
			fileReader.addOrUpdateFile(extraPath, extraContent);
			logger.info("Additional source: " + extraUnit);
		}
	}
	compiler.setSources(sources);

	// Configure EVM version. Default is cancun; `--evm-version <name>`
	// overrides — accepts any solc-supported name (homestead..osaka). The
	// test runner translates fixture-side directives (`// EVMVersion: ...`)
	// to a concrete name and passes the flag.
	auto evmVer = solidity::langutil::EVMVersion::cancun();
	if (!opts.evmVersion.empty())
	{
		using V = solidity::langutil::EVMVersion;
		static std::map<std::string, V> const namedVersions = {
			{"homestead",        V::homestead()},
			{"tangerineWhistle", V::tangerineWhistle()},
			{"spuriousDragon",   V::spuriousDragon()},
			{"byzantium",        V::byzantium()},
			{"constantinople",   V::constantinople()},
			{"petersburg",       V::petersburg()},
			{"istanbul",         V::istanbul()},
			{"berlin",           V::berlin()},
			{"london",           V::london()},
			{"paris",            V::paris()},
			{"shanghai",         V::shanghai()},
			{"cancun",           V::cancun()},
			{"prague",           V::prague()},
			{"osaka",            V::osaka()},
		};
		auto it = namedVersions.find(opts.evmVersion);
		if (it != namedVersions.end())
			evmVer = it->second;
		else
			logger.warning("Unknown EVM version '" + opts.evmVersion + "'; defaulting to cancun");
	}
	compiler.setEVMVersion(evmVer);
	puyasol::builder::setCompileEVMVersion(evmVer);
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
	auto roots = builder.build(compiler, sourceFile, opts.opupBudget, opts.ensureBudget, opts.viaYulBehavior);

	if (roots.empty())
	{
		logger.error("No contracts found");
		return 1;
	}

	logger.info("Generated " + std::to_string(roots.size()) + " AWST root node(s)");

	// ─── --uros-splitter: split AWST into main + N chunks ───────────────
	// Each --uros-splitter flag invocation defines one chunk's method
	// list. The splitter returns mainRoots (replaces `roots`) plus per-
	// chunk root sets that are emitted later via emitChunkAwsts and
	// compileChunksAndEmitDeployTemplate. All chunk-specific machinery
	// lives in UrosSplitter — main.cpp only orchestrates.
	puyasol::splitter::UrosSplitter::Result splitResult;
	if (!opts.urosSplitGroups.empty())
	{
		std::vector<std::set<std::string>> splitGroups;
		for (auto const& g : opts.urosSplitGroups)
			splitGroups.emplace_back(g.begin(), g.end());
		splitResult = puyasol::splitter::UrosSplitter::split(roots, splitGroups);
		roots = std::move(splitResult.mainRoots);
	}

	// ─── Serialization and output ─────────────────────────────────────────

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

	// Write options.json (with template var declarations for child contracts)
	auto const& childContracts = puyasol::builder::sol_ast::SolNewExpression::childContracts();
	std::string optionsPath = (fs::path(opts.outputDir) / "options.json").string();
	// When --uros-splitter is active, the stub bodies reference a
	// TemplateVar(UROS_ORCH_APP_ID); declare it as an integer template
	// var so puya doesn't reject the AWST. Default 0 acts as a placeholder
	// — the deploy harness substitutes the real orchestrator app id.
	std::map<std::string, int64_t> intTemplateVars;
	if (!splitResult.chunks.empty())
		intTemplateVars["UROS_ORCH_APP_ID"] = opts.urosOrchAppId;
	if (contractNames.size() <= 1)
	{
		std::string contractName = contractNames.empty() ? "" : contractNames[0];
		puyasol::json::OptionsWriter::write(optionsPath, contractName, opts.outputDir, opts.optimizationLevel, opts.outputIr, childContracts, intTemplateVars);
	}
	else
	{
		puyasol::json::OptionsWriter::writeMultiple(optionsPath, contractNames, opts.outputDir, opts.optimizationLevel, opts.outputIr, childContracts, intTemplateVars);
	}
	logger.info("Wrote: " + optionsPath);

	// ─── --uros-splitter: emit per-chunk AWST + options eagerly ─────────
	// Done before the --no-puya gate so inspection/manual puya runs
	// work even when puya invocation is skipped.
	auto chunkPaths = puyasol::splitter::UrosSplitter::emitChunkAwsts(
		opts.outputDir, splitResult,
		opts.optimizationLevel, opts.outputIr, opts.urosOrchAppId);

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

		// Generate .tmpl file if any child contracts were referenced via new C()
		auto const& children = puyasol::builder::sol_ast::SolNewExpression::childContracts();
		if (!children.empty())
		{
			std::string tmplPath = (fs::path(opts.outputDir) / "deploy.tmpl.json").string();
			njson tmpl = njson::object();
			for (auto const& childName : children)
			{
				// Read the child's compiled binaries from the output dir
				auto approvalBin = fs::path(opts.outputDir) / (childName + ".approval.bin");
				auto clearBin = fs::path(opts.outputDir) / (childName + ".clear.bin");
				if (fs::exists(approvalBin))
				{
					std::ifstream af(approvalBin, std::ios::binary);
					std::vector<uint8_t> ab((std::istreambuf_iterator<char>(af)),
						std::istreambuf_iterator<char>());
					std::string hex;
					for (auto b : ab)
					{
						char buf[3];
						snprintf(buf, sizeof(buf), "%02x", b);
						hex += buf;
					}
					tmpl["TMPL_APPROVAL_" + childName] = hex;
				}
				if (fs::exists(clearBin))
				{
					std::ifstream cf(clearBin, std::ios::binary);
					std::vector<uint8_t> cb((std::istreambuf_iterator<char>(cf)),
						std::istreambuf_iterator<char>());
					std::string hex;
					for (auto b : cb)
					{
						char buf[3];
						snprintf(buf, sizeof(buf), "%02x", b);
						hex += buf;
					}
					tmpl["TMPL_CLEAR_" + childName] = hex;
				}
			}
			std::ofstream tf(tmplPath);
			tf << tmpl.dump(2);
			logger.info("Wrote: " + tmplPath);
			puyasol::builder::sol_ast::SolNewExpression::resetChildContracts();
		}

		// ─── --uros-splitter: per-chunk puya pass + deploy template ─────
		if (!splitResult.chunks.empty() && exitCode == 0)
		{
			// Match UrosSplitter::findPrimaryContract — iterate in
			// reverse to pick the LAST Contract (the deployable target,
			// per Solidity convention).
			std::string mainBareName;
			for (auto it = roots.rbegin(); it != roots.rend(); ++it)
				if (auto const* c = dynamic_cast<puyasol::awst::Contract const*>(it->get()))
					{ mainBareName = c->name; break; }

			int rc = puyasol::splitter::UrosSplitter::compileChunksAndEmitDeployTemplate(
				opts.outputDir, mainBareName, splitResult, chunkPaths,
				opts.puyaPath, opts.logLevel);
			if (rc != 0) return rc;
		}

		return exitCode;
	}

	logger.info("Done! AWST JSON generated. Use --puya-path to compile to TEAL.");
	return 0;
}
