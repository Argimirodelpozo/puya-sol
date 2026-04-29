#include "Logger.h"
#include "builder/AWSTBuilder.h"
#include "builder/assembly/AssemblyBuilder.h"
#include "builder/sol-ast/calls/SolNewExpression.h"
#include "json/AWSTSerializer.h"
#include "json/OptionsWriter.h"
#include "runner/PuyaRunner.h"
#include "splitter/SimpleSplitter.h"

#include <libsolidity/interface/CompilerStack.h>
#include <libsolidity/interface/FileReader.h>
#include <libsolidity/interface/ImportRemapper.h>
#include <liblangutil/CharStreamProvider.h>

#include <boost/filesystem.hpp>

#include <fstream>
#include <nlohmann/json.hpp>
using njson = nlohmann::ordered_json;
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
	bool splitTest = false;
	bool viaYulBehavior = false;
	std::string upstreamTestDir; // for ExternalSource resolution
	std::string splitConfig; // path to JSON file with subroutine names to extract
	// Experimental: comma-separated list of function names to extract using
	// the delegate-update mechanism (per-call temporary approval-program
	// swap on the orchestrator). Each named function gets its own "lonely
	// chunk" sidecar contract and is removed from the orchestrator entirely.
	std::vector<std::string> forceDelegate;
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
		<< "  --split-test           Split a semantic test file (Source/ExternalSource directives)\n"
		<< "                         into individual .sol files in output-dir. Prints main source path.\n"
		<< "  --upstream-test-dir <d> Upstream Solidity semanticTests dir (for ExternalSource)\n"
		<< "  --split-config <path>  JSON file: { \"extract\": [\"FuncName.A\", ...] } —\n"
		<< "                         names of subroutines to peel off into a sibling helper\n"
		<< "                         contract (one helper per run). Names not found in the\n"
		<< "                         AWST are silently ignored, so one config can target\n"
		<< "                         multiple contract families.\n"
		<< "  --force-delegate <list>  EXPERIMENTAL: comma-separated function names to extract\n"
		<< "                         via the delegate-update mechanism. Each function gets a\n"
		<< "                         dedicated sidecar app that, when invoked, temporarily\n"
		<< "                         swaps the orchestrator's approval program with F's body\n"
		<< "                         (so F runs against the orchestrator's storage), invokes\n"
		<< "                         the orchestrator, then reverts the swap. Caller must\n"
		<< "                         submit a group [orch.stub, sidecar].\n"
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
		else if (arg == "--split-test")
			opts.splitTest = true;
		else if (arg == "--upstream-test-dir" && i + 1 < _argc)
			opts.upstreamTestDir = _argv[++i];
		else if (arg == "--split-config" && i + 1 < _argc)
			opts.splitConfig = _argv[++i];
		else if (arg == "--force-delegate" && i + 1 < _argc)
		{
			std::string raw = _argv[++i];
			std::string token;
			auto pushToken = [&]() {
				if (token.empty()) return;
				if (token == "__postInit")
					std::cerr << "warning: --force-delegate refuses '__postInit' "
						"(constructor; the delegate-update mechanism cannot be "
						"used during deploy)\n";
				else
					opts.forceDelegate.push_back(token);
				token.clear();
			};
			for (char c : raw)
			{
				if (c == ',') pushToken();
				else if (!std::isspace(static_cast<unsigned char>(c))) token += c;
			}
			pushToken();
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

	// Set up log file output (on by default)
	if (opts.outputLogs)
	{
		fs::create_directories(opts.outputDir);
		std::string logPath = (fs::path(opts.outputDir) / "puya-sol.log").string();
		logger.setOutputLogFile(logPath);
	}

	if (opts.sourceFiles.empty())
	{
		std::cerr << "Error: --source is required" << std::endl;
		printUsage(_argv[0]);
		return 1;
	}

	if (!opts.noPuya && !opts.splitTest && opts.puyaPath.empty())
	{
		std::cerr << "Error: --puya-path is required (or use --no-puya)" << std::endl;
		return 1;
	}

	// Resolve absolute path (first source is the "main" source)
	fs::path sourceAbsPath = fs::absolute(opts.sourceFiles[0]);
	std::string sourceFile = sourceAbsPath.string();

	// --split-test mode: parse Source/ExternalSource directives, write split files
	if (opts.splitTest)
	{
		std::ifstream ifs(sourceFile);
		if (!ifs)
		{
			std::cerr << "Error: cannot open " << sourceFile << std::endl;
			return 1;
		}
		fs::create_directories(opts.outputDir);

		// Determine upstream test dir for ExternalSource resolution
		fs::path upstreamDir;
		if (!opts.upstreamTestDir.empty())
			upstreamDir = fs::path(opts.upstreamTestDir);

		// If not provided, infer from test file's category relative to upstream
		// semanticTests dir: solidity/test/libsolidity/semanticTests/<category>/
		if (upstreamDir.empty())
		{
			// Try to find it by looking for the solidity fork
			fs::path srcDir = fs::path(sourceFile).parent_path();
			// Walk up to find a "tests" or similar directory
			upstreamDir = srcDir; // fallback: same directory
		}

		std::string mainSource;
		std::string line;
		std::string currentName;
		std::string currentContent;
		std::map<std::string, std::string> sources;

		auto flushCurrent = [&]() {
			if (!currentName.empty() || !currentContent.empty())
			{
				// Strip test assertions
				auto pos = currentContent.find("// ----");
				if (pos != std::string::npos)
					currentContent = currentContent.substr(0, pos);
				// Strip ==== lines
				std::string cleaned;
				std::istringstream ss(currentContent);
				std::string l;
				while (std::getline(ss, l))
				{
					if (l.find("==== ") == 0 && l.rfind(" ====") == l.size() - 5)
						continue;
					cleaned += l + "\n";
				}
				if (currentName.empty())
					currentName = fs::path(sourceFile).filename().string();
				sources[currentName] = cleaned;
				mainSource = currentName;
			}
		};

		while (std::getline(ifs, line))
		{
			// ==== Source: name ====
			if (line.find("==== Source: ") == 0 && line.rfind(" ====") == line.size() - 5)
			{
				flushCurrent();
				currentName = line.substr(12, line.size() - 17); // strip delimiters
				// Trim whitespace
				while (!currentName.empty() && currentName.front() == ' ') currentName.erase(0, 1);
				while (!currentName.empty() && currentName.back() == ' ') currentName.pop_back();
				currentContent.clear();
			}
			// ==== ExternalSource: spec ====
			else if (line.find("==== ExternalSource: ") == 0 && line.rfind(" ====") == line.size() - 5)
			{
				std::string spec = line.substr(21, line.size() - 26);
				while (!spec.empty() && spec.front() == ' ') spec.erase(0, 1);
				while (!spec.empty() && spec.back() == ' ') spec.pop_back();

				std::string importName, fsPath;
				auto eqPos = spec.find('=');
				if (eqPos != std::string::npos)
				{
					importName = spec.substr(0, eqPos);
					fsPath = spec.substr(eqPos + 1);
				}
				else
				{
					importName = spec;
					fsPath = spec;
				}
				// Trim
				while (!importName.empty() && importName.front() == ' ') importName.erase(0, 1);
				while (!importName.empty() && importName.back() == ' ') importName.pop_back();
				while (!fsPath.empty() && fsPath.front() == ' ') fsPath.erase(0, 1);
				while (!fsPath.empty() && fsPath.back() == ' ') fsPath.pop_back();

				// Resolve external source from upstream test directory
				fs::path extPath = upstreamDir / fsPath;
				if (fs::exists(extPath))
				{
					std::ifstream extIfs(extPath.string());
					std::string extContent((std::istreambuf_iterator<char>(extIfs)),
						std::istreambuf_iterator<char>());
					// Sanitize import name for filesystem
					std::string safeName = importName;
					while (!safeName.empty() && safeName.front() == '/') safeName.erase(0, 1);
					while (safeName.find("../") == 0) safeName.erase(0, 3);
					sources[safeName] = extContent;
					// Write immediately
					fs::path dst = fs::path(opts.outputDir) / safeName;
					fs::create_directories(dst.parent_path());
					std::ofstream(dst.string()) << extContent;
				}
				else
				{
					std::cerr << "Warning: ExternalSource not found: " << extPath << std::endl;
				}
			}
			else
			{
				currentContent += line + "\n";
			}
		}
		flushCurrent();

		// Write all sources to output dir
		for (auto const& [name, content]: sources)
		{
			fs::path dst = fs::path(opts.outputDir) / name;
			fs::create_directories(dst.parent_path());
			std::ofstream(dst.string()) << content;
		}

		// Output: main source path
		std::cout << (fs::path(opts.outputDir) / mainSource).string() << std::endl;
		return 0;
	}

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

	// Auto-detect `// compileViaYul: true` directive in semantic test files
	// so we match Solidity's viaIR codegen semantics (modifier `_` invocations
	// get fresh locals) without requiring the test runner to pass a flag.
	if (!opts.viaYulBehavior)
	{
		static std::regex const viaYulRe(R"(//\s*compileViaYul\s*:\s*true\b)");
		if (std::regex_search(rawMainSource, viaYulRe))
		{
			opts.viaYulBehavior = true;
			logger.debug("Detected compileViaYul:true directive — enabling viaIR codegen");
		}
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

	// ─── Helper extraction (from --split-config or hardcoded fallback) ────
	// Subroutine names to peel off into a sibling helper contract, keeping
	// the orchestrator under AVM's 8192-byte cap. Each named sub is moved to
	// a sibling app the orchestrator calls via inner app-call. Names match
	// `Subroutine.name` in the AWST (lib-prefixed where applicable).
	//
	// Config file format (JSON):
	//   { "extract": ["FuncName.A", "FuncName.B", ...] }
	//
	// When --split-config isn't passed, the hardcoded fallback below applies
	// (covers v1 and v2 ctf-exchange families; names not present in the
	// current AWST are silently filtered out).
	std::vector<std::string> kExtractedFunctions = {
		// ── solady signature + ECDSA (v1 + v2) ────────────────────────────
		"SignatureCheckerLib.isValidSignatureNow",
		"ECDSA.tryRecover",            // tuple-return overloads auto-skipped
		"ECDSA.recover",
		"ECDSA.toTypedDataHash",
		"ECDSA._throwError",
		// ── Polymarket proxy/safe CREATE2 (v1 + v2) ───────────────────────
		// v1 uses *_computeCreationCode + *_computeCreate2Address
		"PolyProxyLib._computeCreationCode",
		"PolyProxyLib._computeCreate2Address",
		"PolyProxyLib.getProxyWalletAddress",
		"PolySafeLib._computeCreate2Address",
		"PolySafeLib.getSafeAddress",
		"PolySafeLib.getContractBytecode",
		// v2 uses *_computeCreationCodeHash + Create2Lib + _getSalt
		"PolyProxyLib._computeCreationCodeHash",
		"PolyProxyLib._getSalt",
		"PolySafeLib._computeCreationCodeHash",
		"PolySafeLib._getSalt",
		"PolySafeLib.getSafeWalletAddress",
		"PolySafeLib.computeBytecodeHash",
		"Create2Lib.computeCreate2Address",
		// ── solmate / solady transfer wrappers (v1 + v2) ──────────────────
		"SafeTransferLib.safeTransferFrom",
		"SafeTransferLib.safeTransfer",
		"TransferHelper._transferFromERC1155",
		"TransferHelper._transferFromERC20",
		// `_transferERC20(token, to, value)` lowers to `IERC20.transfer(to, value)`,
		// whose msg.sender at the receiver IS the caller. In the EVM that's
		// `address(this)` (the exchange — libraries inline at the call site).
		// Extracting this stub to helper1 puts msg.sender at usdc = helper1,
		// which has no balance — so the from==this branch of
		// `_transferCollateral` (used by mint/merge/exchange-intermediate
		// settlement paths) silently breaks. Keep `_transferERC20` inline
		// in the orch so msg.sender resolves to the exchange itself; the
		// non-self path uses `_transferFromERC20` which is allowance-based
		// and works regardless of msg.sender.
		// "TransferHelper._transferERC20",
		// ── CalculatorHelper (v1 + v2) ────────────────────────────────────
		"CalculatorHelper.calculateTakingAmount",
		"CalculatorHelper.calculateFee",
		"CalculatorHelper.min",
		"CalculatorHelper.calculatePrice",
		"CalculatorHelper._calculatePrice",
		"CalculatorHelper.isCrossing",
		"CalculatorHelper._isCrossing",
		"_deriveMatchType",
		// ── v2 CTF math (Conditional Tokens helpers) ──────────────────────
		"CTHelpers.sqrt",
		"CTHelpers.getCollectionId",
		"CTHelpers.getPositionId",
	};

	// Helper-list form: each entry is one helper contract's extraction list.
	// Single-helper config translates to one entry. The runtime peels helpers
	// off in order, accumulating helper contracts and threading the
	// orchestrator's roots forward as the input to the next split.
	std::vector<std::vector<std::string>> kHelperExtractions;

	// If --split-config <path> was passed, replace the fallback with the
	// file's contents. Two accepted shapes:
	//   { "extract": ["A", "B", ...] }                       (one helper)
	//   { "helpers": [ {"extract": [...]}, {"extract": [...]} ] }  (N helpers)
	if (!opts.splitConfig.empty())
	{
		std::ifstream cf(opts.splitConfig);
		if (!cf.is_open())
		{
			logger.error("Cannot open --split-config file: " + opts.splitConfig);
			return 1;
		}
		std::ostringstream ss;
		ss << cf.rdbuf();
		try
		{
			auto cfg = njson::parse(ss.str());
			kExtractedFunctions.clear();
			if (cfg.contains("helpers") && cfg["helpers"].is_array())
			{
				for (auto const& h : cfg["helpers"])
				{
					std::vector<std::string> names;
					if (h.contains("extract") && h["extract"].is_array())
						for (auto const& e : h["extract"])
							if (e.is_string()) names.push_back(e.get<std::string>());
					kHelperExtractions.push_back(std::move(names));
				}
				size_t total = 0;
				for (auto const& v : kHelperExtractions) total += v.size();
				logger.info(
					"Loaded " + std::to_string(total) + " extraction name(s) across "
					+ std::to_string(kHelperExtractions.size()) + " helper(s) from "
					+ opts.splitConfig);
			}
			else if (cfg.contains("extract") && cfg["extract"].is_array())
			{
				for (auto const& e : cfg["extract"])
					if (e.is_string())
						kExtractedFunctions.push_back(e.get<std::string>());
				kHelperExtractions.push_back(kExtractedFunctions);
				logger.info(
					"Loaded " + std::to_string(kExtractedFunctions.size()) +
					" extraction name(s) from " + opts.splitConfig);
			}
		}
		catch (std::exception const& e)
		{
			logger.error("Failed to parse --split-config: " + std::string(e.what()));
			return 1;
		}
	}
	else
	{
		// Fallback path: single helper from the hardcoded list.
		kHelperExtractions.push_back(kExtractedFunctions);
	}

	// Resolve delegate names against the AWST. Names not present are warned
	// and dropped. Each surviving name becomes its own one-function helper
	// extraction appended to kHelperExtractions; that's enough to remove F
	// from the orchestrator and measure the size win. The runtime mechanism
	// (lonely-chunk + UpdateApplication dance) is still landing — until then
	// the helper for a delegated F holds F's body. We DON'T compile that
	// helper through puya (its body has unresolved InstanceMethodTarget
	// refs to orchestrator-internal methods); the artifact will be replaced
	// with a hand-crafted lonely chunk in a later pass.
	std::set<std::string> delegateFunctionNames;
	if (!opts.forceDelegate.empty())
	{
		std::set<std::string> presentAll;
		for (auto const& r : roots)
		{
			if (auto sub = std::dynamic_pointer_cast<puyasol::awst::Subroutine>(r))
				presentAll.insert(sub->name);
			else if (auto c = std::dynamic_pointer_cast<puyasol::awst::Contract>(r))
				for (auto const& m : c->methods)
					presentAll.insert(m.memberName);
		}
		int delegateCount = 0;
		for (auto const& name : opts.forceDelegate)
		{
			if (!presentAll.count(name))
			{
				logger.warning("--force-delegate: '" + name + "' not found in AWST, skipping");
				continue;
			}
			kHelperExtractions.push_back({name});
			delegateFunctionNames.insert(name);
			delegateCount++;
		}
		if (delegateCount)
			logger.info(
				"--force-delegate: " + std::to_string(delegateCount)
				+ " function(s) routed to dedicated sidecar helpers (one per F)");
	}

	std::vector<puyasol::splitter::SimpleSplitter::ContractAWST> splitContracts;
	{
		// Walk through each helper-spec in order. After each split() call, the
		// resulting orchestrator becomes the input to the next split, peeling
		// off one helper at a time.
		std::vector<std::shared_ptr<puyasol::awst::RootNode>> currentRoots = roots;
		int helperIdx = 1;
		for (auto const& names : kHelperExtractions)
		{
			std::set<std::string> present;
			for (auto const& r : currentRoots)
			{
				if (auto sub = std::dynamic_pointer_cast<puyasol::awst::Subroutine>(r))
					present.insert(sub->name);
				else if (auto c = std::dynamic_pointer_cast<puyasol::awst::Contract>(r))
					for (auto const& m : c->methods)
						present.insert(m.memberName);
			}
			std::vector<std::string> toExtract;
			for (auto const& name : names)
				if (present.count(name))
					toExtract.push_back(name);
			if (toExtract.empty()) continue;

			logger.info("Extracting " + std::to_string(toExtract.size()) +
				" function(s) into helper #" + std::to_string(helperIdx));

			puyasol::splitter::SimpleSplitter splitter;
			auto parts = splitter.split(currentRoots, toExtract, helperIdx, opts.ensureBudget);
			if (parts.empty())
			{
				logger.warning("Splitter pass " + std::to_string(helperIdx) +
					" returned no result; halting further splits");
				break;
			}
			// parts[0] = helper, parts[1] = orchestrator.
			splitContracts.push_back(parts.front());
			currentRoots = parts.back().roots;
			helperIdx++;
		}

		// Final orchestrator goes last in splitContracts.
		if (!splitContracts.empty())
		{
			puyasol::splitter::SimpleSplitter::ContractAWST orch;
			// Locate the primary Contract pointer to copy its id/name out.
			//
			// Strategy: prefer the contract whose name matches the source
			// file's stem (e.g. `CollateralOnramp` for `CollateralOnramp.sol`).
			// When the entry-point source imports another file that contributes
			// concrete contracts (e.g. `CollateralOnramp.sol` imports
			// `CollateralToken from "./CollateralToken.sol"`), the imported
			// contract may land later in the AWST roots than the file's own
			// declaration, so a pure last-match heuristic ends up picking the
			// import. Match-by-stem disambiguates without depending on root
			// ordering.
			//
			// Fallback: if no name matches the stem, keep the last non-helper
			// Contract. Solidity resolves contracts from least-derived to
			// most-derived within a compilation unit, so the last is usually
			// the most concrete declaration.
			fs::path srcPath(sourceFile);
			std::string sourceStem = srcPath.stem().string();
			std::shared_ptr<puyasol::awst::Contract> stemMatch;
			std::shared_ptr<puyasol::awst::Contract> lastMatch;
			for (auto const& r : currentRoots)
			{
				if (auto c = std::dynamic_pointer_cast<puyasol::awst::Contract>(r))
				{
					if (c->name.find("__Helper") != std::string::npos) continue;
					lastMatch = c;
					if (c->name == sourceStem) stemMatch = c;
				}
			}
			auto chosen = stemMatch ? stemMatch : lastMatch;
			if (chosen)
			{
				orch.contractId = chosen->id;
				orch.contractName = chosen->name;
			}
			orch.roots = std::move(currentRoots);
			splitContracts.push_back(std::move(orch));
		}
	}

	// ─── Serialization and output ─────────────────────────────────────────

	// Serialize to JSON
	puyasol::json::AWSTSerializer serializer;

	// Create output directory
	fs::create_directories(opts.outputDir);

	// Multi-contract path: one subdir per split partition, each its own
	// awst.json / options.json / puya invocation.
	if (!splitContracts.empty())
	{
		// Helper contracts come first in splitContracts; collect their names
		// so the orchestrator's options.json can declare the uint64 template
		// vars (`TMPL_<helperName>_APP_ID`) the orchestrator's stubs reference.
		std::set<std::string> helperNames;
		for (auto const& cawst : splitContracts)
		{
			// The orchestrator's contract id matches `roots`'s primary contract
			// id; everything else is a helper.
			bool isOrch = false;
			for (auto const& r : cawst.roots)
				if (auto c = std::dynamic_pointer_cast<puyasol::awst::Contract>(r))
					if (c->id == cawst.contractId && c->name == cawst.contractName
						&& cawst.contractName.find("__Helper") == std::string::npos)
						isOrch = true;
			if (!isOrch) helperNames.insert(cawst.contractName);
		}

		// Delegate helpers: a sidecar contract whose name contains "__Helper"
		// AND has a method whose memberName is in delegateFunctionNames.
		// These are skipped through puya — their real artifact (the lonely-
		// chunk TEAL) is hand-crafted later. The orchestrator itself, even
		// though it carries a stub for the same memberName, is never skipped.
		std::set<std::string> delegateHelperContractNames;
		for (auto const& cawst : splitContracts)
		{
			if (cawst.contractName.find("__Helper") == std::string::npos) continue;
			for (auto const& r : cawst.roots)
				if (auto c = std::dynamic_pointer_cast<puyasol::awst::Contract>(r))
					for (auto const& m : c->methods)
						if (delegateFunctionNames.count(m.memberName))
							delegateHelperContractNames.insert(cawst.contractName);
		}

		puyasol::runner::PuyaRunner runner;
		runner.setPuyaPath(opts.puyaPath);
		int aggregateExitCode = 0;
		for (auto const& cawst : splitContracts)
		{
			fs::path subdir = fs::path(opts.outputDir) / cawst.contractName;
			fs::create_directories(subdir);

			auto subJson = serializer.serialize(cawst.roots);
			std::string subAwstPath = (subdir / "awst.json").string();
			{
				std::ofstream out(subAwstPath);
				out << subJson.dump(2) << std::endl;
				logger.info("Wrote: " + subAwstPath);
			}
			std::string subOptionsPath = (subdir / "options.json").string();
			// Each contract declares template-var refs to OTHER helpers it
			// might call into. With multi-helper splits a later helper can
			// inherit a stub that targets an earlier helper, so we declare
			// every helper's app id everywhere except in the helper's own
			// options (which would self-reference).
			std::set<std::string> declareVars;
			for (auto const& h : helperNames)
				if (h != cawst.contractName) declareVars.insert(h);
			// puya keys compilation_set by the Contract's full id (file path
			// prefixed FQN), not the short name.
			puyasol::json::OptionsWriter::write(
				subOptionsPath, cawst.contractId, subdir.string(),
				opts.optimizationLevel, opts.outputIr, declareVars);
			logger.info("Wrote: " + subOptionsPath);

			if (!opts.noPuya)
			{
				logger.info("Invoking puya backend for '" + cawst.contractName + "'...");
				int exitCode = runner.run(subAwstPath, subOptionsPath, opts.logLevel);
				if (exitCode != 0)
					aggregateExitCode = exitCode;
			}
		}
		if (logger.warningCount() > 0)
			logger.info("Completed with " + std::to_string(logger.warningCount()) + " warning(s)");
		return aggregateExitCode;
	}

	auto awstJson = serializer.serialize(roots);

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
