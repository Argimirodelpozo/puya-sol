#include "Logger.h"
#include "builder/AWSTBuilder.h"
#include "builder/assembly/AssemblyBuilder.h"
#include "builder/sol-ast/calls/SolNewExpression.h"
#include "json/AWSTSerializer.h"
#include "json/OptionsWriter.h"
#include "runner/PuyaRunner.h"

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

	// Configure EVM version — use Cancun by default, but honour test
	// directives like `// EVMVersion: <=berlin` when present. A test that
	// uses names shadowing newer builtins (e.g. a user `basefee` function
	// on berlin) needs the compiler to pick the older version so the
	// builtin isn't reserved.
	auto evmVer = solidity::langutil::EVMVersion::cancun();
	{
		// Ordered from oldest to newest.
		std::vector<std::pair<std::string, solidity::langutil::EVMVersion>> ladder = {
			{"homestead",        solidity::langutil::EVMVersion::homestead()},
			{"tangerineWhistle", solidity::langutil::EVMVersion::tangerineWhistle()},
			{"spuriousDragon",   solidity::langutil::EVMVersion::spuriousDragon()},
			{"byzantium",        solidity::langutil::EVMVersion::byzantium()},
			{"constantinople",   solidity::langutil::EVMVersion::constantinople()},
			{"petersburg",       solidity::langutil::EVMVersion::petersburg()},
			{"istanbul",         solidity::langutil::EVMVersion::istanbul()},
			{"berlin",           solidity::langutil::EVMVersion::berlin()},
			{"london",           solidity::langutil::EVMVersion::london()},
			{"paris",            solidity::langutil::EVMVersion::paris()},
			{"shanghai",         solidity::langutil::EVMVersion::shanghai()},
			{"cancun",           solidity::langutil::EVMVersion::cancun()},
			{"prague",           solidity::langutil::EVMVersion::prague()},
			{"osaka",            solidity::langutil::EVMVersion::osaka()},
		};
		auto pickIndex = [&](std::string const& _name) -> int {
			for (size_t i = 0; i < ladder.size(); ++i)
				if (ladder[i].first == _name) return static_cast<int>(i);
			return -1;
		};
		// Look for `// EVMVersion: <op?><name>` directive in the main source.
		std::regex directiveRe(R"(//\s*EVMVersion:\s*([<>=!]*)\s*(\w+))");
		std::smatch m;
		if (std::regex_search(mainSourceContent, m, directiveRe))
		{
			std::string op = m[1].str();
			std::string name = m[2].str();
			int idx = pickIndex(name);
			if (idx >= 0)
			{
				// `<=X`, `=X`, bare `X`: pick X
				// `<X`: pick X-1 (previous version)
				// `>=X`, `>X`: bump to that version (or one above) so that
				//              tests requiring newer features (e.g. clz which
				//              needs osaka) can be parsed.
				if (op == "<=" || op.empty() || op == "=" || op == "==")
				{
					evmVer = ladder[idx].second;
				}
				else if (op == "<")
				{
					if (idx > 0)
						evmVer = ladder[idx - 1].second;
				}
				else if (op == ">=" || op == ">")
				{
					int curIdx = pickIndex(evmVer.name());
					int targetIdx = (op == ">") ? idx + 1 : idx;
					if (targetIdx > curIdx && targetIdx < static_cast<int>(ladder.size()))
						evmVer = ladder[targetIdx].second;
				}
			}
		}
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
	if (contractNames.size() <= 1)
	{
		std::string contractName = contractNames.empty() ? "" : contractNames[0];
		puyasol::json::OptionsWriter::write(optionsPath, contractName, opts.outputDir, opts.optimizationLevel, opts.outputIr, childContracts);
	}
	else
	{
		puyasol::json::OptionsWriter::writeMultiple(optionsPath, contractNames, opts.outputDir, opts.optimizationLevel, opts.outputIr, childContracts);
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

		return exitCode;
	}

	logger.info("Done! AWST JSON generated. Use --puya-path to compile to TEAL.");
	return 0;
}
