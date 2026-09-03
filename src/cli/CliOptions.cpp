#include "cli/CliOptions.h"

#include "HexBytes.h"
#include "Logger.h"
#include "builder/EvmFeaturePolicy.h"
#include "builder/ScratchLayout.h"

#include <boost/filesystem.hpp>

#include <cctype>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace fs = boost::filesystem;

namespace puyasol::cli
{

namespace
{
/// Checked numeric option parse: whole-string, non-negative decimal.
/// Malformed input is a fatal usage error with the option named — the bare
/// std::sto* calls previously terminated with an uncaught std::invalid_argument
/// (or silently accepted trailing garbage like "12abc").
unsigned long long parseNumber(std::string const& _opt, std::string const& _val)
{
	if (_val.empty()
		|| _val.find_first_not_of("0123456789") != std::string::npos)
	{
		std::cerr << "Error: " << _opt << " expects a non-negative integer, got '"
			<< _val << "'" << std::endl;
		std::exit(2);
	}
	try
	{
		return std::stoull(_val);
	}
	catch (std::exception const&)
	{
		std::cerr << "Error: " << _opt << " value out of range: '" << _val
			<< "'" << std::endl;
		std::exit(2);
	}
}

int parseBoundedInt(
	std::string const& _opt,
	std::string const& _val,
	unsigned long long _min,
	unsigned long long _max)
{
	auto const value = parseNumber(_opt, _val);
	if (value < _min || value > _max)
	{
		std::cerr << "Error: " << _opt << " expects a value in [" << _min
			<< ", " << _max << "], got '" << _val << "'" << std::endl;
		std::exit(2);
	}
	return static_cast<int>(value);
}

std::string parseUint256Decimal(
	std::string const& _opt, std::string const& _value)
{
	if (_value.empty()
		|| _value.find_first_not_of("0123456789") != std::string::npos)
	{
		std::cerr << "Error: " << _opt << " expects a decimal uint256, got '"
			<< _value << "'" << std::endl;
		std::exit(2);
	}
	auto first = _value.find_first_not_of('0');
	std::string normalized = first == std::string::npos ? "0" : _value.substr(first);
	static constexpr std::string_view maxU256 =
		"115792089237316195423570985008687907853269984665640564039457584007913129639935";
	if (normalized.size() > maxU256.size()
		|| (normalized.size() == maxU256.size() && normalized > maxU256))
	{
		std::cerr << "Error: " << _opt << " value exceeds uint256: '"
			<< _value << "'" << std::endl;
		std::exit(2);
	}
	return normalized;
}

/// Validate a hex BLOB argument at parse time, where every other hex option is
/// already checked. --xchain-template/--xchain-placeholder used to be taken
/// verbatim and decoded later by a lambda that partially parsed "0g" and
/// aborted the process on "gg" (audit H-06). _expectedBytes 0 = any nonzero
/// even length. Returns the normalised lowercase digits.
std::string parseHexBlob(
	std::string const& _opt, std::string value, size_t _expectedBytes)
{
	auto bytes = puyasol::hexToBytes(value, _expectedBytes);
	if (!bytes)
	{
		std::cerr << "Error: " << _opt << " expects "
			<< (_expectedBytes ? std::to_string(_expectedBytes) + " bytes as "
					+ std::to_string(_expectedBytes * 2) + " hex digits"
				: std::string("an even-length run of hex digits"))
			<< " (optional 0x prefix), got '" << value << "'" << std::endl;
		std::exit(2);
	}
	if (value.starts_with("0x") || value.starts_with("0X"))
		value.erase(0, 2);
	for (char& c: value)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return value;
}

std::string parseAddressHex(std::string const& _opt, std::string value)
{
	if (value.starts_with("0x") || value.starts_with("0X"))
		value.erase(0, 2);
	if (value.size() != 40)
	{
		std::cerr << "Error: " << _opt
			<< " expects exactly 20 address bytes (40 hex digits), got '"
			<< value << "'" << std::endl;
		std::exit(2);
	}
	for (char& c: value)
	{
		if (!std::isxdigit(static_cast<unsigned char>(c)))
		{
			std::cerr << "Error: " << _opt << " expects a hex address, got '"
				<< value << "'" << std::endl;
			std::exit(2);
		}
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return value;
}
} // namespace

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
		<< "  --evm-memory-slots <N> Scratch slots for EVM memory, contiguous from slot 0 (default 5 = 20KB,\n"
		<< "                         max " << builder::ScratchLayout::maxMemorySlots
		<< "; UltraHonk needs ~32). Transient/flash reservations follow at N..N+10\n"
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
		<< "  --evm-selectors        Expose keccak-based Solidity function/event selectors,\n"
		<< "                         interface IDs, msg.sig, and selector-bearing ABI values.\n"
		<< "                         ARC-4 selectors remain the route in the ARC-4 profile.\n"
		<< "  --contract-abi <mode>  Contract entry/return wire ABI: arc4 (default) or evm.\n"
		<< "                         EVM mode takes selector in ApplicationArgs[0] and one\n"
		<< "                         canonical ABI body blob in ApplicationArgs[1].\n"
		<< "  --evm-version <name>   EVM version for the Solidity parser. Accepts the same\n"
		<< "                         names solc supports: homestead..osaka. Default: cancun.\n"
		<< "  --xchain-template <hex>  xchain LogicSig template bytecode (20-byte owner\n"
		<< "                         placeholder inside; see --xchain-placeholder). Enables\n"
		<< "                         the xchain account model in the EVM profile.\n"
		<< "  --xchain-placeholder <hex>  The 20-byte owner placeholder inside the\n"
		<< "                         template (default ee x20).\n"
		<< "  --child-programs-via-box  `new C()` child approval programs load from a\n"
		<< "                         deployer-provisioned __cp_<Child> box instead of\n"
		<< "                         embedded template constants (16KB-cap relief).\n"
		<< "  --evm-chain-id <N>     Compile-time uint256 returned by block.chainid. Without\n"
		<< "                         it, GenesisHash is used as an AVM network identity.\n"
		<< "  --evm-block-gas-limit <N> Compile-time uint256 returned by block.gaslimit.\n"
		<< "                         Without it, current OpcodeBudget is used.\n"
		<< "  --evm-coinbase <addr>  Compile-time 20-byte hex block.coinbase value. Required\n"
		<< "                         by sources that read coinbase; AVM has no native analog.\n"
		<< "  --allow-divergence <name>  Explicitly acknowledge one non-EVM lowering. Repeatable.\n"
		<< "                         Valid names: "
		<< builder::EvmFeaturePolicy::allowedNames() << "\n"
		<< "  --force-inline-sub <Name>  Set inlineOpt=true on every Subroutine or\n"
		<< "                         ContractMethod whose name matches <Name>. Puya inlines\n"
		<< "                         the body at every call site. Repeatable.\n"
		<< "  --force-no-inline-sub <Name>  Set inlineOpt=false and retain a real\n"
		<< "                         subroutine for the matching name. Repeatable.\n"
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
		{
			opts.logLevel = _argv[++i];
			if (opts.logLevel != "debug" && opts.logLevel != "info"
				&& opts.logLevel != "warning" && opts.logLevel != "error")
			{
				std::cerr << "Error: --log-level expects debug, info, warning, or "
					"error; got '" << opts.logLevel << "'" << std::endl;
				std::exit(2);
			}
		}
		else if (arg == "--dump-awst")
			opts.dumpAwst = true;
		else if (arg == "--no-puya")
			opts.noPuya = true;
		else if (arg == "--opup-budget" && i + 1 < _argc)
			opts.opupBudget = parseNumber("--opup-budget", _argv[++i]);
		else if (arg == "--ensure-budget" && i + 1 < _argc)
		{
			// Format: func_name:budget
			std::string spec = _argv[++i];
			auto colon = spec.find(':');
			if (colon == std::string::npos || colon == 0)
			{
				std::cerr << "Error: --ensure-budget expects <function>:<budget>, got '"
					<< spec << "'" << std::endl;
				std::exit(2);
			}
			opts.ensureBudget[spec.substr(0, colon)] =
				parseNumber("--ensure-budget", spec.substr(colon + 1));
		}
		else if (arg == "--optimization-level" && i + 1 < _argc)
			opts.optimizationLevel = parseBoundedInt(
				"--optimization-level", _argv[++i], 0, 2);
		else if (arg == "--evm-memory-slots" && i + 1 < _argc)
			opts.evmMemorySlots = parseBoundedInt(
				"--evm-memory-slots", _argv[++i], 1,
				builder::ScratchLayout::maxMemorySlots);
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
		else if (arg == "--evm-selectors")
			opts.evmSelectors = true;
		else if (arg == "--contract-abi" && i + 1 < _argc)
		{
			opts.contractAbi = _argv[++i];
			if (opts.contractAbi != "arc4" && opts.contractAbi != "evm")
			{
				std::cerr << "Error: --contract-abi expects arc4 or evm; got '"
					<< opts.contractAbi << "'" << std::endl;
				std::exit(2);
			}
		}
		else if (arg == "--evm-version" && i + 1 < _argc)
			opts.evmVersion = _argv[++i];
		else if (arg == "--evm-chain-id" && i + 1 < _argc)
			opts.evmChainId = parseUint256Decimal(arg, _argv[++i]);
		else if (arg == "--evm-block-gas-limit" && i + 1 < _argc)
			opts.evmBlockGasLimit = parseUint256Decimal(arg, _argv[++i]);
		else if (arg == "--evm-coinbase" && i + 1 < _argc)
			opts.evmCoinbase = parseAddressHex(arg, _argv[++i]);
		else if (arg == "--allow-divergence" && i + 1 < _argc)
		{
			std::string name = _argv[++i];
			if (!builder::EvmFeaturePolicy::isAllowName(name))
			{
				std::cerr << "Error: --allow-divergence does not recognize '"
					<< name << "'. Valid names: "
					<< builder::EvmFeaturePolicy::allowedNames() << std::endl;
				std::exit(2);
			}
			opts.allowedEvmDivergences.insert(std::move(name));
		}
		else if (arg == "--xchain-template" && i + 1 < _argc)
			opts.xchainTemplateHex = parseHexBlob(arg, _argv[++i], 0);
		else if (arg == "--xchain-placeholder" && i + 1 < _argc)
			opts.xchainPlaceholderHex = parseHexBlob(arg, _argv[++i], 20);
		else if (arg == "--child-programs-via-box")
			opts.childProgramsViaBox = true;
		else if (arg == "--force-inline-sub" && i + 1 < _argc)
			opts.forceInlineSubs.push_back(_argv[++i]);
		else if (arg == "--force-no-inline-sub" && i + 1 < _argc)
			opts.forceNoInlineSubs.push_back(_argv[++i]);
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
