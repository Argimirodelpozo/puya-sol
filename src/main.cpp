/// @file main.cpp
/// Fixed compiler pipeline. Scoped owners retain solc, callback and AWST lifetimes.

#include "Logger.h"
#include "builder/contract/AWSTBuilder.h"
#include "builder/types/EncodedSize.h"
#include "cli/AwstPostPasses.h"
#include "cli/CliOptions.h"
#include "cli/CompilerSetup.h"
#include "json/AWSTSerializer.h"
#include "runner/PuyaRunner.h"

#include <boost/filesystem.hpp>
#include <exception>
#include <iostream>
#include <system_error>

namespace
{
using namespace puyasol;
using namespace puyasol::cli;

// Only the CLI owns process signal policy. The handler does no work beyond
// setting a flag; PuyaRunner kills the backend group and reaps its child.
struct BackendSignals
{
	static inline volatile std::sig_atomic_t pending = 0;
	struct sigaction interrupt{}, terminate{};
	BackendSignals()
	{
		pending = 0;
		struct sigaction action{};
		action.sa_handler = [](int signal) { pending = signal; };
		::sigemptyset(&action.sa_mask);
		if (::sigaction(SIGINT, &action, &interrupt))
			throw std::system_error(errno, std::generic_category(), "install SIGINT handler");
		if (::sigaction(SIGTERM, &action, &terminate))
		{
			auto error = errno;
			::sigaction(SIGINT, &interrupt, nullptr);
			throw std::system_error(error, std::generic_category(), "install SIGTERM handler");
		}
	}
	~BackendSignals()
	{
		::sigaction(SIGINT, &interrupt, nullptr);
		::sigaction(SIGTERM, &terminate, nullptr);
	}
};

int compile(Options const& opts, char const* program)
{
	auto& logger = Logger::instance();
	if (opts.sourceFiles.empty())
	{
		logger.error("--source is required");
		printUsage(program);
		return 1;
	}
	if (!opts.noPuya && opts.puyaPath.empty())
	{
		logger.error("--puya-path is required (or use --no-puya)");
		return 1;
	}
	auto settings = resolveCompilerSettings(opts);
	if (!settings) return 2;

	ArtifactPublisher artifacts(opts);
	if (!artifacts.begin())
	{
		logger.error(artifacts.error());
		return 1;
	}
	logger.info("puya-sol v0.1.0 — Solidity to Algorand Compiler");
	// Destruction is reversed: roots, builder, compiler, then its input/callback.
	SourceInput input(opts);
	solidity::frontend::CompilerStack compiler(input.reader());
	if (!input.load(compiler)) return 1;
	compiler.setEVMVersion(settings->evmVersion);
	logger.info("EVM version set to: " + settings->evmVersion.name());
	logger.info("Parsing and type-checking...");
	bool success = compiler.parseAndAnalyze();
	reportCompilerDiagnostics(compiler);
	if (opts.legacySourceRewrite)
	{
		auto path = boost::filesystem::path(opts.outputDir) / "source-rewrite-manifest.json";
		std::string error;
		if (!writeSourceRewriteManifest(path, input.rewrites(), error))
		{
			logger.error("Legacy source rewrite manifest: " + error);
			return 1;
		}
		logger.info("Wrote: " + path.string());
	}
	// Never lower partial solc semantic annotations.
	if (!success || logger.hasErrors())
	{
		logger.error("Compilation failed.");
		return 1;
	}
	logger.info("Parse and type-check successful!");
	logger.info("Building AWST...");
	builder::AWSTBuilder builder;
	auto roots = builder.build(compiler, input.mainFile(), opts.opupBudget,
		opts.ensureBudget, input.aliases(), std::move(settings->target));
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
	applyInlineOverrides(roots, opts);
	if (logger.hasErrors()) return 1;
	auto serialized = json::AWSTSerializer{}.serialize(roots).dump(2) + '\n';
	auto const& children = builder.artifacts().childContracts;
	if (!artifacts.writeFrontend(serialized, roots, children))
	{
		logger.error("Cannot publish frontend artifacts: " + artifacts.error());
		return 1;
	}
	if (opts.dumpAwst) std::cout << serialized;
	if (!opts.noPuya)
	{
		logger.info("Invoking puya backend...");
		runner::PuyaRunner runner(opts.puyaPath);
		BackendSignals signals;
		int exitCode = runner.run(artifacts.awstPath().string(),
			artifacts.optionsPath().string(), opts.logLevel, {{}, {}, &signals.pending}).exitCode();
		if (exitCode != 0) return exitCode;
		if (!artifacts.finishBackend(children))
		{
			logger.error("Backend artifact validation failed: " + artifacts.error());
			return 1;
		}
		logger.info("Backend artifacts validated successfully");
	}
	else logger.info("Done! AWST JSON generated. Use --puya-path to compile to TEAL.");
	if (logger.warningCount())
		logger.info("Completed with " + std::to_string(logger.warningCount()) + " warning(s)");
	return 0;
}
} // namespace

int main(int argc, char* argv[])
{
	try
	{
		auto options = puyasol::cli::parseArgs(argc, argv);
		puyasol::cli::configureLogger(options);
		return compile(options, argv[0]);
	}
	catch (puyasol::builder::SizeError const& error)
	{
		puyasol::Logger::instance().error(error.what());
	}
	catch (boost::filesystem::filesystem_error const& error)
	{
		puyasol::Logger::instance().error(std::string("Filesystem error: ") + error.what());
	}
	catch (std::exception const& error)
	{
		puyasol::Logger::instance().error(std::string("Internal compiler error: ") + error.what());
	}
	catch (...)
	{
		puyasol::Logger::instance().error("Internal compiler error: unknown exception");
	}
	return 1;
}
