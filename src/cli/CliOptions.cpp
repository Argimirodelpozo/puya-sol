#include "cli/CliOptions.h"
#include "Logger.h"

#include <boost/filesystem.hpp>

#include <cctype>
#include <cstdlib>
#include <iostream>

namespace fs = boost::filesystem;

namespace puyasol::cli
{

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
		<< "  --optimization-level <N>   Puya optimization level: 0, 1, 2 (default: 2)\n"
		<< "  --evm-memory-slots <N> Scratch slots for EVM memory (default 5 = 20KB; UltraHonk needs ~32)\n"
		<< "  --evm-layout           FULL EVM data-location semantics: implies both\n"
		<< "                         --evm-storage-layout and --evm-memory-layout (plus the\n"
		<< "                         transient space coherent with them). The recommended\n"
		<< "                         mode for asm-heavy real-world contracts.\n"
		<< "  --evm-memory-layout    Universal blob memory: every asm-touched memory aggregate\n"
		<< "                         is pointer-modeled in the flat blob (EVM layout).\n"
		<< "  --evm-storage-layout   Back all storage with EVM-numbered slots (paged/sparse boxes).\n"
		<< "                         Faithful assembly slot arithmetic; no ARC-56 state decls.\n"
		<< "  --output-ir            Output all intermediate representations (SSA IR, MIR, TEAL)\n"
		<< "  --no-output-logs       Disable writing compilation logs to output directory\n"
		<< "  --via-yul-behavior     Emulate Solidity's viaIR/compileViaYul codegen semantics\n"
		<< "                         (separate subroutines per modifier, fresh vars per _ invocation)\n"
		<< "  --evm-version <name>   EVM version for the Solidity parser. Accepts the same\n"
		<< "                         names solc supports: homestead..osaka. Default: cancun.\n"
		<< "  --fn-split <spec>      Slice a subroutine's body into pieces. Repeatable.\n"
		<< "                         Format: <SubName>:<idx>,<idx>,...:g<N>[:cross]\n"
		<< "                           SubName  — name of the awst::Subroutine to split\n"
		<< "                           idx,...  — statement indices where splits occur\n"
		<< "                           g<N>     — group id; pieces share the suffix _g<N>\n"
		<< "                           cross    — (optional) pieces live on separate chunks\n"
		<< "                                       and chain via orch.dispatch_chain (gload-\n"
		<< "                                       based prologue). Without it, pieces share\n"
		<< "                                       the same txn frame (load 100).\n"
		<< "                         Pieces are named <SubName>__piece_<i>_g<N> and append\n"
		<< "                         to AWST roots. Live variables across split points flow\n"
		<< "                         through scratch slot 100. With :cross, main.cpp also\n"
		<< "                         emits chain_groups.json so the deploy harness can\n"
		<< "                         register the piece chain with orch after deploy.\n"
		<< "  --pin-to-main <list>   Comma-separated method names that MUST stay on the main\n"
		<< "                         contract and never be split into a chunk. Use for methods\n"
		<< "                         that read msg.sender or address(this) (chunks see orch as\n"
		<< "                         sender / __storage as this). Repeatable.\n"
		<< "  --deploy-pure-helpers  Lift each pure (Solidity `pure`) Subroutine into its own\n"
		<< "                         one-method sidecar Contract. Call sites in the rest of\n"
		<< "                         the contract set are rewritten to inner-txn ApplicationCall\n"
		<< "                         the helper. The helper's bytecode leaves the calling\n"
		<< "                         chunks (per-Contract DCE), at the cost of one inner-txn\n"
		<< "                         per call. Each helper gets a TMPL_PURE_HELPER_<name>_<n>\n"
		<< "                         _APP_ID template var the deploy harness substitutes.\n"
		<< "  --pure-helper-split <Sub>:<idx>,...  Slice the lifted pure helper\n"
		<< "                         into pieces at the given statement indices.\n"
		<< "                         Each piece becomes its own sidecar Contract,\n"
		<< "                         called as a chained inner-txn group at use sites.\n"
		<< "                         Use to fit big helpers into the AVM 8 KB cap.\n"
		<< "  --force-inline-sub <Name>  Set inlineOpt=true on every Subroutine or\n"
		<< "                         ContractMethod whose name matches <Name>. Puya inlines\n"
		<< "                         the body at every call site, so the subroutine itself\n"
		<< "                         can be DCE'd from chunks. Useful for breaking subroutine-\n"
		<< "                         reachability closures that bloat FunctionSplitter chunks\n"
		<< "                         (e.g. inlining a heavy non-pure helper so its body can\n"
		<< "                         be sliced via --fn-split). Repeatable.\n"
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
			// Format: func_name:budget
			std::string spec = _argv[++i];
			auto colon = spec.find(':');
			if (colon != std::string::npos)
				opts.ensureBudget[spec.substr(0, colon)] = std::stoull(spec.substr(colon + 1));
		}
		else if (arg == "--optimization-level" && i + 1 < _argc)
			opts.optimizationLevel = std::stoi(_argv[++i]);
		else if (arg == "--evm-memory-slots" && i + 1 < _argc)
			opts.evmMemorySlots = std::stoi(_argv[++i]);
		else if (arg == "--evm-storage-layout")
			opts.evmStorageLayout = true;
		else if (arg == "--evm-memory-layout")
			opts.evmMemoryLayout = true;
		else if (arg == "--evm-layout")
		{
			// The umbrella: full EVM data-location semantics. Storage as
			// EVM-numbered slots, memory as the flat pointer-modeled blob
			// (asm string/bytes arithmetic works), and the transient space
			// coherent with both. The split flags remain for lane-isolated
			// testing.
			opts.evmStorageLayout = true;
			opts.evmMemoryLayout = true;
		}
		else if (arg == "--output-ir")
			opts.outputIr = true;
		else if (arg == "--no-output-logs")
			opts.outputLogs = false;
		else if (arg == "--via-yul-behavior")
			opts.viaYulBehavior = true;
		else if (arg == "--evm-version" && i + 1 < _argc)
			opts.evmVersion = _argv[++i];
		else if (arg == "--deploy-pure-helpers")
			opts.deployPureHelpers = true;
		else if (arg == "--force-inline-sub" && i + 1 < _argc)
			opts.forceInlineSubs.push_back(_argv[++i]);
		else if (arg == "--force-no-inline-sub" && i + 1 < _argc)
			opts.forceNoInlineSubs.push_back(_argv[++i]);
		else if (arg == "--pure-helper-split" && i + 1 < _argc)
		{
			std::string spec = _argv[++i];
			auto colon = spec.find(':');
			if (colon == std::string::npos)
			{
				std::cerr << "ERR: --pure-helper-split needs <Sub>:<idx>,...\n";
				exit(1);
			}
			Options::PureHelperSplitSpec ps;
			ps.subroutineName = spec.substr(0, colon);
			std::string idxs = spec.substr(colon + 1);
			size_t p = 0;
			while (p <= idxs.size())
			{
				size_t comma = idxs.find(',', p);
				size_t end = (comma == std::string::npos) ? idxs.size() : comma;
				std::string tok = idxs.substr(p, end - p);
				if (!tok.empty())
					ps.splitPoints.push_back(std::stoul(tok));
				if (comma == std::string::npos) break;
				p = comma + 1;
			}
			opts.pureHelperSplits.push_back(std::move(ps));
		}
		else if (arg == "--fn-split" && i + 1 < _argc)
		{
			// Format: <Name>:<idx>,...:g<N>[:cross]. 3 or 4 colon-tokens.
			// "cross" sets crossChunk (gload-based prologue for separate uros chunks).
			std::string spec = _argv[++i];
			std::vector<std::string> toks;
			{
				size_t start = 0;
				while (start <= spec.size())
				{
					size_t colon = spec.find(':', start);
					size_t end = (colon == std::string::npos)
						? spec.size() : colon;
					toks.push_back(spec.substr(start, end - start));
					if (colon == std::string::npos) break;
					start = colon + 1;
				}
			}
			if (toks.size() < 3 || toks.size() > 4)
			{
				std::cerr << "--fn-split: malformed spec '" << spec
					<< "' — expected <Name>:<idx>,<idx>,...:g<N>[:cross]"
					<< std::endl;
				std::exit(1);
			}

			Options::FnSplitSpec fnSpec;
			fnSpec.subroutineName = toks[0];

			// Parse comma-separated indices
			std::string const& idxList = toks[1];
			size_t start = 0;
			while (start <= idxList.size())
			{
				size_t comma = idxList.find(',', start);
				size_t end = (comma == std::string::npos)
					? idxList.size() : comma;
				std::string tok = idxList.substr(start, end - start);
				if (!tok.empty())
					fnSpec.splitPoints.push_back(std::stoull(tok));
				if (comma == std::string::npos) break;
				start = comma + 1;
			}

			// Parse group id (must start with 'g')
			std::string const& gTok = toks[2];
			if (gTok.empty() || gTok[0] != 'g')
			{
				std::cerr << "--fn-split: group id must start with 'g' "
					"(got '" << gTok << "')" << std::endl;
				std::exit(1);
			}
			fnSpec.groupId = std::stoi(gTok.substr(1));

			if (toks.size() == 4)
			{
				if (toks[3] != "cross")
				{
					std::cerr << "--fn-split: trailing token must be "
						"literally 'cross' if present (got '"
						<< toks[3] << "')" << std::endl;
					std::exit(1);
				}
				fnSpec.crossChunk = true;
			}

			opts.fnSplits.push_back(std::move(fnSpec));
		}
		else if (arg == "--pin-to-main" && i + 1 < _argc)
		{
			// Comma-separated; repeatable.
			std::string spec = _argv[++i];
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
					opts.pinnedToMain.push_back(std::move(name));
				if (comma == std::string::npos) break;
				start = comma + 1;
			}
		}
		else if (arg == "--split-config" && i + 1 < _argc)
		{
			opts.splitConfig = _argv[++i];
		}
		else if (arg == "--force-delegate" && i + 1 < _argc)
		{
			std::string spec = _argv[++i];
			size_t start = 0;
			while (start <= spec.size())
			{
				size_t comma = spec.find(',', start);
				size_t end = (comma == std::string::npos) ? spec.size() : comma;
				std::string token = spec.substr(start, end - start);
				while (!token.empty() && std::isspace(static_cast<unsigned char>(token.front())))
					token.erase(token.begin());
				while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back())))
					token.pop_back();
				if (!token.empty())
				{
					if (token == "__postInit")
						std::cerr << "warning: --force-delegate refuses '__postInit' "
							"(constructor; the delegate-update mechanism "
							"cannot be used during deploy)" << std::endl;
					else
						opts.forceDelegate.push_back(token);
				}
				if (comma == std::string::npos) break;
				start = comma + 1;
			}
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

void configureLogger(Options const& _opts)
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

} // namespace puyasol::cli
