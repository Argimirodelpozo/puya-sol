/// @file main.cpp
/// puya-sol pipeline: parse args → load sources → CompilerStack
/// → AWST build → post-passes → awst.json + options.json → puya backend.
/// CLI details live in src/cli/ (CliOptions, SourceCompat, CompilerSetup, AwstPostPasses).

#include "ArtifactIO.h"
#include "Logger.h"
#include "HexBytes.h"
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

	// Invalidate the previous run's commit marker and deployment bundle before
	// doing fallible compilation work. Files without a fresh manifest must not
	// be treated as one coherent compiler run.
	auto const outputDir = fs::path(opts.outputDir);
	auto const artifactManifestPath = outputDir / "artifact-manifest.json";
	boost::system::error_code outputError;
	fs::create_directories(outputDir, outputError);
	if (outputError)
	{
		logger.error("Cannot create artifact output directory: "
			+ outputError.message());
		return 1;
	}
	std::string artifactError;
	if (!puyasol::artifact::removeFileIfPresent(
		artifactManifestPath, artifactError)
		|| !prepareChildDeployArtifacts(opts.outputDir, {}, artifactError))
	{
		logger.error("Cannot invalidate previous artifacts: " + artifactError);
		return 1;
	}
	if (!opts.legacySourceRewrite
		&& !puyasol::artifact::removeFileIfPresent(
			outputDir / "source-rewrite-manifest.json", artifactError))
	{
		logger.error("Cannot remove stale source manifest: " + artifactError);
		return 1;
	}

	// Resolve absolute path (first source is the "main" source)
	fs::path sourceAbsPath = fs::absolute(opts.sourceFiles[0]);
	std::string sourceFile = sourceAbsPath.string();

	logger.info("puya-sol v0.1.0 — Solidity to Algorand Compiler");
	logger.info("Source: " + sourceFile);
	if (opts.legacySourceRewrite)
	{
		// This acknowledgement cannot be hidden with --log-level error. The
		// research harness intentionally opts in, but ordinary builds must never
		// mistake transformed legacy text for the source they supplied.
		std::cerr
			<< "WARNING: UNSAFE LEGACY SOURCE REWRITE ENABLED. Solidity source "
				"will be modified before parsing; exact before/after text and hashes "
				"will be written to source-rewrite-manifest.json."
			<< std::endl;
	}

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

	// Original source is the default and is passed byte-for-byte to solc. The
	// compatibility transforms exist only for explicitly acknowledged corpus
	// research and are recorded below for auditability.
	SourceRewriteMap rewriteRecords;
	std::string mainSourceContent = rawMainSource;
	if (opts.legacySourceRewrite)
	{
		auto interfaceEvents =
			collectInterfaceEventsFromImports(rawMainSource, sourceDir);
		mainSourceContent = transformSource(rawMainSource);
		mainSourceContent =
			removeInheritedEvents(mainSourceContent, interfaceEvents);
	}
	fileReader.addOrUpdateFile(sourceAbsPath, mainSourceContent);

	// Get the normalized source unit name for the main file
	std::string sourceUnitName = fileReader.cliPathToSourceUnitName(sourceAbsPath);
	if (opts.legacySourceRewrite)
		rewriteRecords[sourceUnitName] =
			{rawMainSource, mainSourceContent};
	logger.debug("Source unit: " + sourceUnitName);

	// Imported files obey the same policy as explicit sources. In normal mode
	// this callback is a transparent pass-through.
	auto baseReader = fileReader.reader();
	auto sourceReader = [&](std::string const& _kind, std::string const& _path)
		-> solidity::frontend::ReadCallback::Result
	{
		auto result = baseReader(_kind, _path);
		if (result.success && opts.legacySourceRewrite)
		{
			auto original = result.responseOrErrorMessage;
			result.responseOrErrorMessage = transformSource(original);
			rewriteRecords[_path] =
				{std::move(original), result.responseOrErrorMessage};
		}
		return result;
	};

	solidity::frontend::CompilerStack compiler(sourceReader);

	// Set sources — main source + any additional source files
	std::map<std::string, std::string> sources;
	sources[sourceUnitName] = mainSourceContent;
	for (size_t i = 1; i < opts.sourceFiles.size(); ++i)
	{
		fs::path extraPath = fs::absolute(opts.sourceFiles[i]);
		auto extraContentOpt = readSourceFile(extraPath.string());
		if (extraContentOpt)
		{
			std::string extraContent = std::move(*extraContentOpt);
			std::string extraUnit = fileReader.cliPathToSourceUnitName(extraPath);
			if (opts.legacySourceRewrite)
			{
				auto originalExtra = extraContent;
				extraContent = transformSource(extraContent);
				rewriteRecords[extraUnit] =
					{std::move(originalExtra), extraContent};
			}
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
	if (opts.legacySourceRewrite)
	{
		fs::create_directories(opts.outputDir);
		fs::path manifestPath =
			fs::path(opts.outputDir) / "source-rewrite-manifest.json";
		std::string manifestError;
		if (!writeSourceRewriteManifest(
				manifestPath, rewriteRecords, manifestError))
		{
			logger.error("Legacy source rewrite manifest: " + manifestError);
			return 1;
		}
		logger.info("Wrote: " + manifestPath.string());
	}
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

	logger.info("Building AWST...");
	puyasol::builder::AWSTBuilder builder;
	puyasol::builder::TargetProfile targetProfile{
		.evmStorageLayout = opts.evmStorageLayout,
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
		.allowedEvmDivergences = opts.allowedEvmDivergences,
		.childProgramsViaBox = opts.childProgramsViaBox,
		.evmVersionName = evmVer.name(),
		.scratchLayout = puyasol::builder::ScratchLayout(
			opts.evmMemorySlots > 0
				? opts.evmMemorySlots
				: puyasol::builder::ScratchLayout::defaultMemorySlots),
	};
	// --xchain-template: split the supplied LogicSig template at the 20-byte
	// owner placeholder. The derived address commits to the exact program, so
	// any decode/placement error must fail the COMPILE, not the funds.
	if (!opts.xchainTemplateHex.empty())
	{
		// Strict decode (puyasol::hexToBytes): a non-hex nibble or an odd
		// length is an error, never a partially parsed byte, and nothing here
		// can throw. CliOptions already validated both, so a failure means a
		// producer bypassed the CLI — still fail the compile, never the funds.
		auto tmplBytes = puyasol::hexToBytes(opts.xchainTemplateHex);
		if (!tmplBytes)
		{
			logger.error("--xchain-template must be an even-length run of hex "
				"digits (optional 0x prefix)");
			return 2;
		}
		auto phBytes = puyasol::hexToBytes(opts.xchainPlaceholderHex, 20);
		if (!phBytes)
		{
			logger.error("--xchain-placeholder must be exactly 20 bytes "
				"(40 hex digits, optional 0x prefix)");
			return 2;
		}
		auto const& tmpl = *tmplBytes;
		auto const& ph = *phBytes;
		if (opts.contractAbi != "evm")
		{
			logger.error("--xchain-template requires --contract-abi evm "
				"(the xchain account model lives in the 160-bit namespace)");
			return 2;
		}
		auto it = std::search(tmpl.begin(), tmpl.end(), ph.begin(), ph.end());
		if (it == tmpl.end()
			|| std::search(it + 1, tmpl.end(), ph.begin(), ph.end()) != tmpl.end())
		{
			logger.error("--xchain-template must contain the owner placeholder "
				"exactly once");
			return 2;
		}
		targetProfile.xchainAccounts = puyasol::builder::TargetProfile::XchainAccounts{
			{tmpl.begin(), it}, {it + 20, tmpl.end()}};
	}
	auto roots = builder.build(
		compiler, sourceFile, opts.opupBudget, opts.ensureBudget,
		opts.viaYulBehavior, sourceAliases, std::move(targetProfile));

	if (logger.hasErrors())
	{
		logger.error("AWST generation failed.");
		return 1;
	}

	if (roots.empty())
	{
		logger.error("No contracts found");
		return 1;
	}

	logger.info("Generated " + std::to_string(roots.size()) + " AWST root node(s)");

	// Option-driven post-AWST passes.
	applyInlineOverrides(roots, opts);

	// ─── Serialization and output ─────────────────────────────────────────

	// Serialize to JSON
	puyasol::json::AWSTSerializer serializer;
	auto awstJson = serializer.serialize(roots);

	// Remove compiler-owned outputs for current target names before invoking the
	// backend. This is the per-run isolation boundary that makes every output
	// collected below fresh.
	auto const& childContracts = builder.artifacts().childContracts;
	if (!prepareChildDeployArtifacts(
		opts.outputDir, childContracts, artifactError)
		|| !prepareBackendTargetArtifacts(
			opts.outputDir, roots, artifactError))
	{
		logger.error("Cannot prepare artifact output: " + artifactError);
		return 1;
	}

	// Atomically publish validated frontend artifacts. artifact-manifest.json
	// is the commit marker: without it, files in a reused directory must not be
	// treated as one coherent compiler run.
	auto const awstPath = outputDir / "awst.json";
	puyasol::artifact::Digest awstDigest;
	if (!puyasol::artifact::writeJsonAtomically(
		awstPath, awstJson.dump(2) + '\n', awstDigest, artifactError))
	{
		logger.error("Cannot write AWST artifact: " + artifactError);
		return 1;
	}
	logger.info("Wrote: " + awstPath.string());

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

	// Write options.json (with template var declarations for child contracts).
	auto const optionsPath = outputDir / "options.json";
	std::map<std::string, int64_t> intTemplateVars;
	puyasol::artifact::Digest optionsDigest;
	if (!puyasol::json::OptionsWriter::write(
		optionsPath, contractNames, opts.outputDir, opts.optimizationLevel,
		opts.outputIr, childContracts, intTemplateVars, optionsDigest,
		artifactError))
	{
		logger.error("Cannot write options artifact: " + artifactError);
		return 1;
	}
	logger.info("Wrote: " + optionsPath.string());

	std::vector<puyasol::artifact::Record> artifactRecords{
		{awstPath.filename().string(), "frontend-awst", awstDigest},
		{optionsPath.filename().string(), "backend-options", optionsDigest},
	};
	if (opts.legacySourceRewrite)
	{
		auto const sourceManifest = outputDir / "source-rewrite-manifest.json";
		std::vector<std::uint8_t> contents;
		puyasol::artifact::Digest digest;
		if (!puyasol::artifact::readBinary(
			sourceManifest, contents, digest, artifactError))
		{
			logger.error("Cannot record source manifest: " + artifactError);
			return 1;
		}
		artifactRecords.push_back({
			sourceManifest.filename().string(), "source-rewrite-manifest",
			std::move(digest)});
	}
	auto writeArtifactManifest = [&](std::string const& phase) {
		if (!puyasol::artifact::writeManifest(
			artifactManifestPath, phase, artifactRecords, artifactError))
		{
			logger.error("Cannot write artifact manifest: " + artifactError);
			return false;
		}
		logger.info("Wrote: " + artifactManifestPath.string());
		return true;
	};
	if (!writeArtifactManifest(opts.noPuya ? "frontend-only" : "frontend-ready"))
		return 1;

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
		int exitCode = runner.run(
			awstPath.string(), optionsPath.string(), opts.logLevel);

		// Never derive deployment templates from stale .bin files left in a
		// reused output directory when this backend invocation failed.
		if (exitCode != 0)
			return exitCode;
		if (!writeChildDeployTemplates(
			opts.outputDir, childContracts, artifactRecords, artifactError))
		{
			logger.error("Cannot write child deployment artifacts: " + artifactError);
			return 1;
		}
		if (!collectBackendTargetArtifacts(
			opts.outputDir, roots, artifactRecords, artifactError))
		{
			std::string cleanupError;
			puyasol::artifact::removeFileIfPresent(
				outputDir / "deploy.tmpl.json", cleanupError);
			logger.error("Backend artifact validation failed: " + artifactError);
			return 1;
		}
		if (!childContracts.empty())
			logger.info("Wrote: "
				+ (outputDir / "deploy.tmpl.json").string());
		if (!writeArtifactManifest("backend-complete"))
		{
			std::string cleanupError;
			if (!puyasol::artifact::removeFileIfPresent(
				outputDir / "deploy.tmpl.json", cleanupError))
				logger.error("Cannot invalidate deployment template: " + cleanupError);
			return 1;
		}
		logger.info("Backend artifacts validated successfully");
		return 0;
	}

	logger.info("Done! AWST JSON generated. Use --puya-path to compile to TEAL.");
	return 0;
}
