#include "cli/CliOptions.h"

#include "HexBytes.h"
#include "Logger.h"
#include "builder/target/EvmFeaturePolicy.h"
#include "builder/target/ScratchLayout.h"

#include <iomanip>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <libsolutil/CommonData.h>

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
/// even length. Retain the decoded bytes for profile validation.
std::vector<uint8_t> parseHexBlob(
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
	return std::move(*bytes);
}

std::string parseAddressHex(std::string const& _opt, std::string const& value)
{
	return solidity::util::toHex(parseHexBlob(_opt, value, 20));
}

/// One CLI flag: its spelling, argument, help, and
/// the handler that applies it (`_value` is empty for bare flags). Handlers
/// exit with status 2 on a malformed value, as the inline arms always did.
struct FlagSpec
{
	char const* name;
	char const* valueName;
	std::string help;
	void (*apply)(Options& _opts, std::string const& _value);
};

void applyLogLevel(Options& _opts, std::string const& _value)
{
	_opts.logLevel = _value;
	if (_opts.logLevel != "debug" && _opts.logLevel != "info"
		&& _opts.logLevel != "warning" && _opts.logLevel != "error")
	{
		std::cerr << "Error: --log-level expects debug, info, warning, or "
			"error; got '" << _opts.logLevel << "'" << std::endl;
		std::exit(2);
	}
}

void applyEnsureBudget(Options& _opts, std::string const& _spec)
{
	// Format: func_name:budget
	auto colon = _spec.find(':');
	if (colon == std::string::npos || colon == 0)
	{
		std::cerr << "Error: --ensure-budget expects <function>:<budget>, got '"
			<< _spec << "'" << std::endl;
		std::exit(2);
	}
	_opts.ensureBudget[_spec.substr(0, colon)] =
		parseNumber("--ensure-budget", _spec.substr(colon + 1));
}

void applyContractAbi(Options& _opts, std::string const& _value)
{
	_opts.contractAbi = _value;
	if (_opts.contractAbi != "arc4" && _opts.contractAbi != "evm")
	{
		std::cerr << "Error: --contract-abi expects arc4 or evm; got '"
			<< _opts.contractAbi << "'" << std::endl;
		std::exit(2);
	}
}

void applyAllowDivergence(Options& _opts, std::string const& _value)
{
	std::string name = _value;
	if (!builder::EvmFeaturePolicy::isAllowName(name))
	{
		std::cerr << "Error: --allow-divergence does not recognize '"
			<< name << "'. Valid names: "
			<< builder::EvmFeaturePolicy::allowedNames() << std::endl;
		std::exit(2);
	}
	_opts.allowedEvmDivergences.insert(std::move(name));
}

void rejectEvmMemoryLayout(Options&, std::string const&)
{
	std::cerr
		<< "Error: --evm-memory-layout is not implemented and cannot be "
			"enabled. Compilation stopped before source processing."
		<< std::endl;
	std::exit(2);
}

void rejectEvmLayout(Options&, std::string const&)
{
	std::cerr
		<< "Error: --evm-layout is unavailable because its EVM memory "
			"mode is not implemented. Use --evm-storage-layout only when "
			"slot-compatible storage is sufficient."
		<< std::endl;
	std::exit(2);
}

FlagSpec const kFlags[] = {
	{"--source", "<file>",
		"Solidity source file (required, repeatable for multi-file)",
		[](Options& o, std::string const& v) { o.sourceFiles.push_back(v); }},
	{"--import-path", "<path>",
		"Import path for resolving imports (repeatable)",
		[](Options& o, std::string const& v) { o.importPaths.push_back(v); }},
	{"--remapping", "<map>",
		"Import remapping: prefix=target (repeatable)",
		[](Options& o, std::string const& v) { o.remappings.push_back(v); }},
	{"--output-dir", "<dir>",
		"Output directory (default: out)",
		[](Options& o, std::string const& v) { o.outputDir = v; }},
	{"--puya-path", "<path>",
		"Path to puya executable (required unless --no-puya)",
		[](Options& o, std::string const& v) { o.puyaPath = v; }},
	{"--log-level", "<level>",
		"Log level: debug, info, warning, error (default: info)", applyLogLevel},
	{"--dump-awst", "",
		"Dump AWST JSON to stdout",
		[](Options& o, std::string const&) { o.dumpAwst = true; }},
	{"--no-puya", "",
		"Skip puya invocation (only generate JSON)",
		[](Options& o, std::string const&) { o.noPuya = true; }},
	{"--opup-budget", "<N>",
		"Inject ensure_budget(N) into ALL public methods (OpUp)",
		[](Options& o, std::string const& v) {
			o.opupBudget = parseNumber("--opup-budget", v); }},
	{"--ensure-budget", "<f:N>",
		"Inject ensure_budget(N) into function f (repeatable)", applyEnsureBudget},
	{"--optimization-level", "<N>",
		"Puya optimization level: 0, 1, 2 (default: 2)",
		[](Options& o, std::string const& v) {
			o.optimizationLevel = parseBoundedInt("--optimization-level", v, 0, 2); }},
	{"--evm-memory-slots", "<N>",
		"Scratch slots for EVM memory, contiguous from slot 0 (default 5 = 20KB,\n"
			"                         max " + std::to_string(builder::ScratchLayout::maxMemorySlots)
			+ "; UltraHonk needs ~32). Transient/flash reservations follow at N..N+10",
		[](Options& o, std::string const& v) {
			o.evmMemorySlots = parseBoundedInt(
				"--evm-memory-slots", v, 1, builder::ScratchLayout::maxMemorySlots); }},
	{"--evm-storage-layout", "",
		"Back all storage with EVM-numbered slots (paged/sparse boxes).\n"
			"                         Faithful assembly slots; ARC-56 names only immutable cells.",
		[](Options& o, std::string const&) { o.evmStorageLayout = true; }},
	{"--evm-memory-layout", "",
		"UNAVAILABLE: rejected until universal EVM memory is implemented.", rejectEvmMemoryLayout},
	{"--evm-layout", "",
		"UNAVAILABLE: rejected because it includes that memory mode.", rejectEvmLayout},
	{"--output-ir", "",
		"Output all intermediate representations (SSA IR, MIR, TEAL)",
		[](Options& o, std::string const&) { o.outputIr = true; }},
	{"--no-output-logs", "",
		"Disable writing compilation logs to output directory",
		[](Options& o, std::string const&) { o.outputLogs = false; }},
	{"--via-yul-behavior", "",
		"Emulate Solidity's viaIR/compileViaYul codegen semantics\n"
			"                         (separate subroutines per modifier, fresh vars per _ invocation)",
		[](Options& o, std::string const&) { o.viaYulBehavior = true; }},
	{"--legacy-source-rewrite", "",
		"RESEARCH ONLY: opt into pre-0.8 source rewrites.\n"
			"                         Emits source-rewrite-manifest.json with exact before/after text.",
		[](Options& o, std::string const&) { o.legacySourceRewrite = true; }},
	{"--evm-selectors", "",
		"Expose keccak-based Solidity function/event selectors,\n"
			"                         interface IDs, msg.sig, and selector-bearing ABI values.\n"
			"                         ARC-4 selectors remain the route in the ARC-4 profile.",
		[](Options& o, std::string const&) { o.evmSelectors = true; }},
	{"--proxy-adaptation", "",
		"Opt into recognized proxy-to-native-update adaptations.\n"
			"                         Disabled by default; see proxy.md for semantic boundaries.",
		[](Options& o, std::string const&) { o.proxyAdaptation = true; }},
	{"--contract-abi", "<mode>",
		"Contract entry/return wire ABI: arc4 (default) or evm.\n"
			"                         EVM mode takes selector in ApplicationArgs[0] and one\n"
			"                         canonical ABI body blob in ApplicationArgs[1].", applyContractAbi},
	{"--evm-version", "<name>",
		"EVM version for the Solidity parser. Accepts the same\n"
			"                         names solc supports: homestead..osaka. Default: cancun.",
		[](Options& o, std::string const& v) { o.evmVersion = v; }},
	{"--evm-chain-id", "<N>",
		"Compile-time uint256 returned by block.chainid. Without\n"
			"                         it, GenesisHash is used as an AVM network identity.",
		[](Options& o, std::string const& v) {
			o.evmChainId = parseUint256Decimal("--evm-chain-id", v); }},
	{"--evm-block-gas-limit", "<N>",
		"Compile-time uint256 returned by block.gaslimit.\n"
			"                         Without it, current OpcodeBudget is used.",
		[](Options& o, std::string const& v) {
			o.evmBlockGasLimit = parseUint256Decimal("--evm-block-gas-limit", v); }},
	{"--evm-coinbase", "<addr>",
		"Compile-time 20-byte hex block.coinbase value. Required\n"
			"                         by sources that read coinbase; AVM has no native analog.",
		[](Options& o, std::string const& v) {
			o.evmCoinbase = parseAddressHex("--evm-coinbase", v); }},
	{"--allow-divergence", "<name>",
		"Explicitly acknowledge one non-EVM lowering. Repeatable.\n"
			"                         Valid names: "
			+ builder::EvmFeaturePolicy::allowedNames(), applyAllowDivergence},
	{"--xchain-template", "<hex>",
		"xchain LogicSig template bytecode (20-byte owner\n"
			"                         placeholder inside; see --xchain-placeholder). Enables\n"
			"                         the xchain account model in the EVM profile.",
		[](Options& o, std::string const& v) {
			o.xchainTemplate = parseHexBlob("--xchain-template", v, 0); }},
	{"--xchain-placeholder", "<hex>",
		"The 20-byte owner placeholder inside the\n"
			"                         template (default ee x20).",
		[](Options& o, std::string const& v) {
			o.xchainPlaceholder = parseHexBlob("--xchain-placeholder", v, 20); }},
	{"--child-programs-via-box", "",
		"`new C()` child approval programs load from a\n"
			"                         deployer-provisioned __cp_<Child> box instead of\n"
			"                         embedded template constants (16KB-cap relief).",
		[](Options& o, std::string const&) { o.childProgramsViaBox = true; }},
	{"--force-inline-sub", "<Name>",
		"Set inlineOpt=true on every Subroutine or\n"
			"                         ContractMethod whose name matches <Name>. Puya inlines\n"
			"                         the body at every call site. Repeatable.",
		[](Options& o, std::string const& v) { o.forceInlineSubs.push_back(v); }},
	{"--force-no-inline-sub", "<Name>",
		"Set inlineOpt=false and retain a real\n"
			"                         subroutine for the matching name. Repeatable.",
		[](Options& o, std::string const& v) { o.forceNoInlineSubs.push_back(v); }},
	{"--help", "", "Show this help message", nullptr},
};

FlagSpec const* findFlag(std::string const& _arg)
{
	for (auto const& flag: kFlags)
		if (_arg == flag.name)
			return &flag;
	return nullptr;
}
} // namespace

void printUsage(char const* _progName)
{
	std::cout << "Usage: " << _progName << " [options]\n\nOptions:\n";
	for (auto const& flag: kFlags)
	{
		std::string label = std::string("  ") + flag.name;
		if (*flag.valueName) label += std::string(" ") + flag.valueName;
		std::cout << std::left << std::setw(32) << label << " " << flag.help << '\n';
	}
}

Options parseArgs(int _argc, char* _argv[])
{
	Options opts;

	for (int i = 1; i < _argc; ++i)
	{
		std::string arg = _argv[i];

		if (arg == "--help")
		{
			printUsage(_argv[0]);
			std::exit(0);
		}
		auto const* flag = findFlag(arg);
		if (!flag)
		{
			std::cerr << "Unknown option: " << arg << std::endl;
			printUsage(_argv[0]);
			std::exit(1);
		}
		if (*flag->valueName && (i + 1 >= _argc || findFlag(_argv[i + 1])))
		{
			std::cerr << "Error: " << arg << " requires " << flag->valueName << std::endl;
			std::exit(2);
		}
		flag->apply(opts, *flag->valueName ? std::string(_argv[++i]) : std::string());
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
}

} // namespace puyasol::cli
