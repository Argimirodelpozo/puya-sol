#include "Logger.h"
#include "builder/AWSTBuilder.h"
#include "builder/assembly/AssemblyBuilder.h"
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

	// Write options.json
	std::string optionsPath = (fs::path(opts.outputDir) / "options.json").string();
	if (contractNames.size() <= 1)
	{
		std::string contractName = contractNames.empty() ? "" : contractNames[0];
		puyasol::json::OptionsWriter::write(optionsPath, contractName, opts.outputDir, opts.optimizationLevel, opts.outputIr);
	}
	else
	{
		puyasol::json::OptionsWriter::writeMultiple(optionsPath, contractNames, opts.outputDir, opts.optimizationLevel, opts.outputIr);
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
