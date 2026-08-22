/// @file main.cpp
/// puya-sol pipeline: parse args → transform sources → CompilerStack
/// → AWST build → post-passes → awst.json + options.json → puya backend.
/// CLI details live in src/cli/ (CliOptions, SourceCompat, CompilerSetup, AwstPostPasses).

#include "Logger.h"
#include "builder/AWSTBuilder.h"
#include "builder/ScratchLayout.h"
#include "cli/AwstPostPasses.h"
#include "cli/CliOptions.h"
#include "cli/CompilerSetup.h"
#include "cli/SourceCompat.h"
#include "json/AWSTSerializer.h"
#include "json/OptionsWriter.h"
#include "runner/PuyaRunner.h"

#include <libsolidity/interface/CompilerStack.h>

#include <boost/filesystem.hpp>

#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fs = boost::filesystem;

int main(int _argc, char* _argv[])
{
	using namespace puyasol::cli;

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
	// solc's BASE PATH: it decides the main file's source-unit name, and hence
	// what its own relative imports resolve against. The `contracts/Foo.sol →
	// parent` guess only holds for a flat layout; a real verified source tree
	// nests the entry point deep (e.g. src/solc_0.8/polygon/child/sand/X.sol),
	// and guessing there truncates the unit name so `../../../a/B.sol`
	// overshoots the root and the import is "not found". When an explicit
	// --import-path CONTAINS the source, that root is the answer — prefer the
	// shallowest such path. Falls back to the old guess when none applies.
	fs::path projectRoot = sourceDir.parent_path(); // contracts/ → project root
	for (auto const& ip: opts.importPaths)
	{
		fs::path absIp = fs::absolute(ip).lexically_normal();
		auto rel = sourceAbsPath.lexically_normal().lexically_relative(absIp);
		if (rel.empty() || *rel.begin() == "..")
			continue;                                  // source is not under it
		// Only override when the guess would actually TRUNCATE, i.e. the source
		// sits more than one directory below the root. When it is at the root or
		// one level down (the flat temp tree the multisource splitter writes),
		// the old derivation is already right and changing it would renumber
		// every source-unit name for no gain.
		if (std::distance(rel.begin(), rel.end()) <= 2)
			continue;
		if (projectRoot == sourceDir.parent_path()
			|| std::distance(absIp.begin(), absIp.end())
				< std::distance(projectRoot.begin(), projectRoot.end()))
			projectRoot = absIp;
	}
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
	auto interfaceEvents = collectInterfaceEventsFromImports(rawMainSource, sourceDir);

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
		else
		{
			logger.error("Cannot read additional source file: " + extraPath.string());
			return 1;
		}
	}
	compiler.setSources(sources);

	// Configure EVM version (shared with the builder for version-gated lowering).
	auto evmVer = resolveEvmVersion(opts.evmVersion);
	compiler.setEVMVersion(evmVer);
	logger.info("EVM version set to: " + evmVer.name() + " (hasChainID=" + (evmVer.hasChainID() ? "true" : "false") + ")");

	// Apply import remappings (Foundry-style: prefix=target)
	applyRemappings(compiler, fileReader, opts.remappings);

	logger.info("Parsing and type-checking...");

	// Builder consumes solc's completed semantic annotations as an invariant.
	// An AST from failed analysis is not safe to lower: types, referenced
	// declarations, virtual targets, and call graphs may be unset or partial.
	bool success = compiler.parseAndAnalyze();
	if (!success)
	{
		reportCompilationErrors(compiler);
		logger.error("Compilation failed.");
		return 1;
	}

	logger.info("Parse and type-check successful!");
	logger.debug("Source units: " + std::to_string(compiler.sourceNames().size()));

	// Alias absolute entry paths to solc source-unit names for offset-only
	// source locations. CompilationSession owns the actual CharStream map.
	std::map<std::string, std::string> sourceAliases;
	sourceAliases[sourceFile] = sourceUnitName;
	for (size_t i = 1; i < opts.sourceFiles.size(); ++i)
	{
		fs::path extraPath = fs::absolute(opts.sourceFiles[i]);
		std::string extraUnit = fileReader.cliPathToSourceUnitName(extraPath);
		if (sources.count(extraUnit))
			sourceAliases[extraPath.string()] = extraUnit;
	}

	// Build AWST
	logger.info("Building AWST...");
	puyasol::builder::AWSTBuilder builder;
	puyasol::builder::TargetProfile targetProfile{
		.evmStorageLayout = opts.evmStorageLayout,
		.evmMemoryLayout = opts.evmMemoryLayout,
		.evmSelectors = opts.evmSelectors || opts.contractAbi == "evm",
		.contractAbi = opts.contractAbi == "evm"
			? puyasol::builder::ContractAbi::Evm
			: puyasol::builder::ContractAbi::Arc4,
		.viaIRSequencing = opts.viaYulBehavior,
		.evmChainId = opts.evmChainId.empty()
			? std::nullopt : std::optional<std::string>{opts.evmChainId},
		.evmBlockGasLimit = opts.evmBlockGasLimit.empty()
			? std::nullopt : std::optional<std::string>{opts.evmBlockGasLimit},
		.evmCoinbase = opts.evmCoinbase.empty()
			? std::nullopt : std::optional<std::string>{opts.evmCoinbase},
		.evmVersionName = evmVer.name(),
		.scratchLayout = puyasol::builder::ScratchLayout(
			opts.evmMemorySlots > 0
				? opts.evmMemorySlots
				: puyasol::builder::ScratchLayout::defaultMemorySlots),
	};
	auto roots = builder.build(
		compiler, sourceFile, opts.opupBudget, opts.ensureBudget,
		opts.viaYulBehavior, sourceAliases, std::move(targetProfile));

	if (roots.empty())
	{
		logger.error("No contracts found");
		return 1;
	}

	logger.info("Generated " + std::to_string(roots.size()) + " AWST root node(s)");

	// Option-driven post-AWST passes (order matters; see AwstPostPasses.h).
	applyInlineOverrides(roots, opts);
	applyFnSplits(roots, opts);
	auto pureHelperResult = extractPureHelpers(roots, opts);
	if (auto exitCode = runSimpleSplitterIfRequested(roots, opts, sourceFile))
		return *exitCode;

	// ─── Serialization and output ─────────────────────────────────────────

	// Serialize to JSON
	puyasol::json::AWSTSerializer serializer;
	auto awstJson = serializer.serialize(roots);

	// Create output directory
	fs::create_directories(opts.outputDir);

	// Write awst.json — and verify the write. A silently truncated awst.json
	// (full disk, permissions) would otherwise feed the backend a stale or
	// partial file from a reused output directory.
	std::string awstPath = (fs::path(opts.outputDir) / "awst.json").string();
	{
		std::ofstream out(awstPath);
		out << awstJson.dump(2) << std::endl;
		out.close();
		if (!out)
		{
			logger.error("Failed writing " + awstPath);
			return 1;
		}
		logger.info("Wrote: " + awstPath);
	}

	// Assigning INTO a constant is never meaningful — it means some lvalue path
	// gave up and returned a placeholder (e.g. SolExpressionDispatch's
	// "unsupported member access" warning yields an empty BytesConstant), so the
	// write silently goes nowhere. puya rejects it with an unreadable
	// "deserialization failed: 'BytesConstant'" because a constant isn't in its
	// Lvalue union; other shapes would just drop the store. Fail loud here, at
	// the source location, instead of either outcome.
	{
		static const std::set<std::string> kConstNodes{
			"IntegerConstant", "BoolConstant", "BytesConstant", "StringConstant",
			"VoidConstant", "MethodConstant", "AddressConstant"};
		std::function<void(nlohmann::json const&)> scan = [&](nlohmann::json const& node)
		{
			if (node.is_object())
			{
				auto ty = node.value("_type", std::string{});
				if ((ty == "AssignmentExpression" || ty == "AssignmentStatement")
					&& node.contains("target") && node["target"].is_object()
					&& kConstNodes.count(node["target"].value("_type", std::string{})))
				{
					auto const& t = node["target"];
					puyasol::awst::SourceLocation loc;
					if (node.contains("source_location") && node["source_location"].is_object())
					{
						auto const& sl = node["source_location"];
						loc.file = sl.value("file", std::string{});
						loc.line = sl.value("line", 0);
					}
					logger.error(
						"assignment target lowered to a constant ("
						+ t.value("_type", std::string{})
						+ ") — this write would be silently dropped. The left-hand side "
						"uses a construct puya-sol cannot resolve to storage or memory "
						"(look for a preceding 'unsupported member access' warning).",
						loc);
				}
				for (auto const& [k, v]: node.items())
					scan(v);
			}
			else if (node.is_array())
				for (auto const& v: node)
					scan(v);
		};
		scan(awstJson);
		if (logger.hasErrors())
			return 1;
	}


	// Dump to stdout if requested (keep on stdout for piping)
	if (opts.dumpAwst)
		std::cout << awstJson.dump(2) << std::endl;

	// Get all contract / logic-sig names for the compilation set.
	std::vector<std::string> contractNames;
	for (auto const& root: roots)
	{
		if (auto const* contract = dynamic_cast<puyasol::awst::Contract const*>(root.get()))
			contractNames.push_back(contract->id);
		else if (auto const* lsig = dynamic_cast<puyasol::awst::LogicSignature const*>(root.get()))
			contractNames.push_back(lsig->id);
	}

	// Write options.json (with template var declarations for child contracts)
	auto const& childContracts = builder.artifacts().childContracts;
	std::string optionsPath = (fs::path(opts.outputDir) / "options.json").string();
	std::map<std::string, int64_t> intTemplateVars;
	// --deploy-pure-helpers injects TemplateVars at rewritten call sites;
	// declare as int placeholders (deploy harness substitutes real app ids).
	for (auto const& h : pureHelperResult.extracted)
		intTemplateVars[h.templateVarName] = 0;
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

		// Never derive deployment templates from stale .bin files left in a
		// reused output directory when this backend invocation failed.
		if (exitCode == 0)
			writeChildDeployTemplates(opts.outputDir, childContracts);

		return exitCode;
	}

	logger.info("Done! AWST JSON generated. Use --puya-path to compile to TEAL.");
	return 0;
}
