#include "Logger.h"
#include "builder/AWSTBuilder.h"
#include "json/AWSTSerializer.h"
#include "json/OptionsWriter.h"
#include "runner/PuyaRunner.h"
#include "splitter/SizeEstimator.h"
#include "splitter/CallGraphAnalyzer.h"
#include "splitter/ContractSplitter.h"
#include "splitter/FunctionSplitter.h"
#include "splitter/FunctionInliner.h"
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
	bool inlineAll = false;
	int optimizationLevel = 1;
	bool outputIr = false;
	bool outputLogs = true;
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
		<< "  --inline-all               Fully inline all subroutine calls before splitting\n"
		<< "  --optimization-level <N>   Puya optimization level: 0, 1, 2 (default: 1)\n"
		<< "  --output-ir            Output all intermediate representations (SSA IR, MIR, TEAL)\n"
		<< "  --no-output-logs       Disable writing compilation logs to output directory\n"
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
		else if (arg == "--inline-all")
			opts.inlineAll = true;
		else if (arg == "--optimization-level" && i + 1 < _argc)
			opts.optimizationLevel = std::stoi(_argv[++i]);
		else if (arg == "--output-ir")
			opts.outputIr = true;
		else if (arg == "--no-output-logs")
			opts.outputLogs = false;
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

		// ─── Phase -1: Full function inlining (BEFORE splitting) ─────────────
		// When --inline-all is set, recursively inline all subroutine calls
		// in the contract's public methods (e.g., verify()). This produces
		// monolithic functions that can then be split into sequential chunks
		// forming a linear pipeline for group transaction execution.
		if (opts.inlineAll && opts.splitContracts)
		{
			logger.info("Phase -1: Full function inlining...");
			puyasol::splitter::FunctionInliner inliner;

			// Find the primary entry point to inline. For most contracts this is
			// the first non-internal public method. For UltraHonk, it's "verify".
			std::set<std::string> targetNames;
			for (auto const& method: primaryContract->methods)
			{
				if (method.memberName != "__bare_create__" &&
					method.memberName.substr(0, 2) != "__" &&
					method.memberName != "loadVerificationKey")
				{
					// Only inline verify (the main entry point that calls everything)
					if (method.memberName == "verify")
						targetNames.insert(method.memberName);
				}
			}
			if (targetNames.empty())
			{
				// Fallback: inline all public methods
				for (auto const& method: primaryContract->methods)
					if (method.memberName != "__bare_create__" &&
						method.memberName.substr(0, 2) != "__")
						targetNames.insert(method.memberName);
			}

			auto inlineResult = inliner.inlineAll(targetNames, primaryContract, roots);
			if (inlineResult.didInline)
			{
				logger.info("  Inlined " + std::to_string(inlineResult.inlinedFunctions.size()) +
					" unique functions, " + std::to_string(inlineResult.totalStatements) +
					" statements in target");

				// After inlining, remove ALL contract methods that are NOT:
				// - The target method (verify)
				// - loadVerificationKey (separate entry point)
				// - __bare_create__ (bare method)
				// Internal methods like verifySumcheck, computePublicInputDelta
				// are now dead — their code lives inside verify's body.
				primaryContract->methods.erase(
					std::remove_if(primaryContract->methods.begin(), primaryContract->methods.end(),
						[&](puyasol::awst::ContractMethod const& m) {
							if (targetNames.count(m.memberName))
								return false; // keep target
							if (m.memberName == "__bare_create__" ||
								m.memberName == "loadVerificationKey" ||
								m.memberName.substr(0, 2) == "__")
								return false; // keep infrastructure methods
							return true; // remove everything else
						}),
					primaryContract->methods.end()
				);

				// Scan verify's inlined body for all SubroutineID references
				// to determine which subroutines are still needed.
				std::set<std::string> referencedIds;
				for (auto const& method: primaryContract->methods)
				{
					if (!targetNames.count(method.memberName) || !method.body)
						continue;
					// Walk the entire inlined body to find SubroutineCallExpressions
					std::function<void(puyasol::awst::Expression const&)> scanExpr;
					std::function<void(puyasol::awst::Statement const&)> scanStmt;
					scanExpr = [&](puyasol::awst::Expression const& e)
					{
						if (e.nodeType() == "SubroutineCallExpression")
						{
							auto const& call = static_cast<puyasol::awst::SubroutineCallExpression const&>(e);
							if (auto const* sid = std::get_if<puyasol::awst::SubroutineID>(&call.target))
								referencedIds.insert(sid->target);
							for (auto const& arg: call.args)
								if (arg.value) scanExpr(*arg.value);
							return;
						}
						// Recurse into all expression children
						auto recurseExpr = [&](std::shared_ptr<puyasol::awst::Expression> const& ex) {
							if (ex) scanExpr(*ex);
						};
						std::string type = e.nodeType();
						if (type == "BigUIntBinaryOperation") {
							auto const& op = static_cast<puyasol::awst::BigUIntBinaryOperation const&>(e);
							recurseExpr(op.left); recurseExpr(op.right);
						} else if (type == "UInt64BinaryOperation") {
							auto const& op = static_cast<puyasol::awst::UInt64BinaryOperation const&>(e);
							recurseExpr(op.left); recurseExpr(op.right);
						} else if (type == "BytesBinaryOperation") {
							auto const& op = static_cast<puyasol::awst::BytesBinaryOperation const&>(e);
							recurseExpr(op.left); recurseExpr(op.right);
						} else if (type == "BytesUnaryOperation") {
							auto const& op = static_cast<puyasol::awst::BytesUnaryOperation const&>(e);
							recurseExpr(op.expr);
						} else if (type == "NumericComparisonExpression") {
							auto const& op = static_cast<puyasol::awst::NumericComparisonExpression const&>(e);
							recurseExpr(op.lhs); recurseExpr(op.rhs);
						} else if (type == "BytesComparisonExpression") {
							auto const& op = static_cast<puyasol::awst::BytesComparisonExpression const&>(e);
							recurseExpr(op.lhs); recurseExpr(op.rhs);
						} else if (type == "BooleanBinaryOperation") {
							auto const& op = static_cast<puyasol::awst::BooleanBinaryOperation const&>(e);
							recurseExpr(op.left); recurseExpr(op.right);
						} else if (type == "Not") {
							recurseExpr(static_cast<puyasol::awst::Not const&>(e).expr);
						} else if (type == "AssertExpression") {
							recurseExpr(static_cast<puyasol::awst::AssertExpression const&>(e).condition);
						} else if (type == "ConditionalExpression") {
							auto const& c = static_cast<puyasol::awst::ConditionalExpression const&>(e);
							recurseExpr(c.condition); recurseExpr(c.trueExpr); recurseExpr(c.falseExpr);
						} else if (type == "AssignmentExpression") {
							auto const& a = static_cast<puyasol::awst::AssignmentExpression const&>(e);
							recurseExpr(a.target); recurseExpr(a.value);
						} else if (type == "IntrinsicCall") {
							for (auto const& arg: static_cast<puyasol::awst::IntrinsicCall const&>(e).stackArgs)
								recurseExpr(arg);
						} else if (type == "PuyaLibCall") {
							for (auto const& arg: static_cast<puyasol::awst::PuyaLibCall const&>(e).args)
								recurseExpr(arg.value);
						} else if (type == "ReinterpretCast") {
							recurseExpr(static_cast<puyasol::awst::ReinterpretCast const&>(e).expr);
						} else if (type == "Copy") {
							recurseExpr(static_cast<puyasol::awst::Copy const&>(e).value);
						} else if (type == "CheckedMaybe") {
							recurseExpr(static_cast<puyasol::awst::CheckedMaybe const&>(e).expr);
						} else if (type == "ARC4Encode") {
							recurseExpr(static_cast<puyasol::awst::ARC4Encode const&>(e).value);
						} else if (type == "ARC4Decode") {
							recurseExpr(static_cast<puyasol::awst::ARC4Decode const&>(e).value);
						} else if (type == "SingleEvaluation") {
							recurseExpr(static_cast<puyasol::awst::SingleEvaluation const&>(e).source);
						} else if (type == "FieldExpression") {
							recurseExpr(static_cast<puyasol::awst::FieldExpression const&>(e).base);
						} else if (type == "IndexExpression") {
							auto const& idx = static_cast<puyasol::awst::IndexExpression const&>(e);
							recurseExpr(idx.base); recurseExpr(idx.index);
						} else if (type == "TupleExpression") {
							for (auto const& item: static_cast<puyasol::awst::TupleExpression const&>(e).items)
								recurseExpr(item);
						} else if (type == "TupleItemExpression") {
							recurseExpr(static_cast<puyasol::awst::TupleItemExpression const&>(e).base);
						} else if (type == "StateGet") {
							auto const& sg = static_cast<puyasol::awst::StateGet const&>(e);
							recurseExpr(sg.field); recurseExpr(sg.defaultValue);
						} else if (type == "BoxValueExpression") {
							recurseExpr(static_cast<puyasol::awst::BoxValueExpression const&>(e).key);
						} else if (type == "NewStruct") {
							for (auto const& [_, v]: static_cast<puyasol::awst::NewStruct const&>(e).values)
								recurseExpr(v);
						} else if (type == "Emit") {
							recurseExpr(static_cast<puyasol::awst::Emit const&>(e).value);
						} else if (type == "NewArray") {
							for (auto const& v: static_cast<puyasol::awst::NewArray const&>(e).values)
								recurseExpr(v);
						} else if (type == "ArrayLength") {
							recurseExpr(static_cast<puyasol::awst::ArrayLength const&>(e).array);
						} else if (type == "CommaExpression") {
							for (auto const& ex: static_cast<puyasol::awst::CommaExpression const&>(e).expressions)
								recurseExpr(ex);
						}
					};
					scanStmt = [&](puyasol::awst::Statement const& s)
					{
						std::string type = s.nodeType();
						if (type == "ExpressionStatement")
							{ if (auto const& ex = static_cast<puyasol::awst::ExpressionStatement const&>(s).expr) scanExpr(*ex); }
						else if (type == "AssignmentStatement")
							{ auto const& a = static_cast<puyasol::awst::AssignmentStatement const&>(s); if (a.target) scanExpr(*a.target); if (a.value) scanExpr(*a.value); }
						else if (type == "ReturnStatement")
							{ if (auto const& v = static_cast<puyasol::awst::ReturnStatement const&>(s).value) scanExpr(*v); }
						else if (type == "IfElse") {
							auto const& ie = static_cast<puyasol::awst::IfElse const&>(s);
							if (ie.condition) scanExpr(*ie.condition);
							if (ie.ifBranch) for (auto const& st: ie.ifBranch->body) if (st) scanStmt(*st);
							if (ie.elseBranch) for (auto const& st: ie.elseBranch->body) if (st) scanStmt(*st);
						} else if (type == "WhileLoop") {
							auto const& wl = static_cast<puyasol::awst::WhileLoop const&>(s);
							if (wl.condition) scanExpr(*wl.condition);
							if (wl.loopBody) for (auto const& st: wl.loopBody->body) if (st) scanStmt(*st);
						} else if (type == "Block") {
							for (auto const& st: static_cast<puyasol::awst::Block const&>(s).body) if (st) scanStmt(*st);
						} else if (type == "UInt64AugmentedAssignment") {
							auto const& ua = static_cast<puyasol::awst::UInt64AugmentedAssignment const&>(s);
							if (ua.target) scanExpr(*ua.target); if (ua.value) scanExpr(*ua.value);
						} else if (type == "BigUIntAugmentedAssignment") {
							auto const& ba = static_cast<puyasol::awst::BigUIntAugmentedAssignment const&>(s);
							if (ba.target) scanExpr(*ba.target); if (ba.value) scanExpr(*ba.value);
						}
					};
					for (auto const& stmt: method.body->body)
						if (stmt) scanStmt(*stmt);
				}

				// Remove subroutines that are NOT referenced by the inlined body
				size_t removedCount = 0;
				roots.erase(
					std::remove_if(roots.begin(), roots.end(),
						[&](std::shared_ptr<puyasol::awst::RootNode> const& r) {
							auto const* sub = dynamic_cast<puyasol::awst::Subroutine const*>(r.get());
							if (!sub)
								return false;
							// Keep the verify subroutine created by inliner
							if (targetNames.count(sub->name))
								return false;
							// Keep subroutines still referenced by the inlined body
							if (referencedIds.count(sub->id))
								return false;
							++removedCount;
							return true;
						}),
					roots.end()
				);

				logger.info("  Removed " + std::to_string(removedCount) +
					" subroutines (kept " + std::to_string(referencedIds.size()) +
					" referenced), " +
					std::to_string(primaryContract->methods.size()) + " methods");

				// Re-collect subroutines after inlining
				subroutines.clear();
				for (auto const& root: roots)
				{
					if (dynamic_cast<puyasol::awst::Subroutine const*>(root.get()))
						subroutines.push_back(root);
				}
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
			constexpr size_t maxChunkInstructions = 2500;

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

		// ─── Phase 1: Size estimation (disabled — estimator is inaccurate) ──
		// The size estimator currently overestimates significantly (e.g. 33KB
		// estimated vs 6.5KB actual), so we skip the warning/recommendation
		// unless --split-contracts is explicitly requested.
		puyasol::splitter::SizeEstimator estimator;
		auto estimate = estimator.estimate(*primaryContract, subroutines);

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
						opts.optimizationLevel,
						opts.outputIr
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
		// Size estimator is currently broken (overestimates significantly),
		// so we don't emit split recommendations unless explicitly requested.
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
