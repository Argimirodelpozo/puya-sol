#include "cli/CompilerSetup.h"
#include "Logger.h"

#include <libsolidity/interface/ImportRemapper.h>
#include <liblangutil/SourceReferenceFormatter.h>

#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <vector>

namespace fs = boost::filesystem;

namespace puyasol::cli
{
namespace
{

std::map<std::string, std::string> sourceFileAliases(
	solidity::frontend::FileReader const& reader)
{
	using Reader = solidity::frontend::FileReader;
	std::map<std::string, std::string> result;
	auto prefixes = reader.includePaths();
	prefixes.push_back(reader.basePath());
	for (auto const& [unit, _]: reader.sourceUnits())
	{
		// FileReader has no public resolved-path map. Match its normalized
		// base/include candidates only after a successful read, never basenames.
		std::string path = unit.starts_with("file://") ? unit.substr(7) : unit;
		std::set<fs::path> candidates;
		for (auto const& prefix: prefixes)
		{
			auto candidate = Reader::normalizeCLIPathForVFS(
				prefix / path, Reader::SymlinkResolution::Enabled);
			if (fs::is_regular_file(candidate)) candidates.insert(std::move(candidate));
		}
		if (candidates.size() == 1) result.emplace(candidates.begin()->string(), unit);
	}
	return result;
}

solidity::frontend::FileReader setupFileReader(
	Options const& _opts,
	fs::path const& _mainPath)
{
	auto const _sourceDir = _mainPath.parent_path();
	// Preserve the explicit-root policy; solc owns all source-unit normalization.
	std::optional<fs::path> importRoot;
	for (auto const& path: _opts.importPaths)
	{
		auto root = fs::absolute(path).lexically_normal();
		auto relative = _mainPath.lexically_normal().lexically_relative(root);
		if (relative.empty() || *relative.begin() == "..") continue;
		if (!importRoot || std::distance(root.begin(), root.end())
			< std::distance(importRoot->begin(), importRoot->end()))
			importRoot = std::move(root);
	}
	auto const _projectRoot = importRoot.value_or(_sourceDir.parent_path());
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

	// Resolve stdlib roots via /proc/self/exe (no-op on non-procfs platforms).
	// CMake stages the supported layout next to a build-tree executable and
	// installs it one directory above bin/:
	//   <exe-dir>/share/puya-sol/       — build tree
	//   <exe-dir>/../share/puya-sol/    — installed tree
	// Keep the source-tree fallback for existing developer builds and WIP
	// example imports.
	try
	{
		char execPathBuf[4096];
		ssize_t len = ::readlink("/proc/self/exe", execPathBuf, sizeof(execPathBuf) - 1);
		if (len > 0)
		{
			execPathBuf[len] = '\0';
			fs::path exeDir = fs::path(execPathBuf).parent_path();
			std::vector<fs::path> libsBases{
				exeDir / "share" / "puya-sol",
				exeDir.parent_path() / "share" / "puya-sol",
				exeDir.parent_path() / "src",
			};
			for (auto const& libsBase: libsBases)
			{
				if (fs::exists(libsBase / "libs"))
				{
					fileReader.addIncludePath(libsBase);
					fileReader.allowDirectory(libsBase);
					break;
				}
			}
			// Example contracts (tokens/, examples/)
			fs::path stdlibBase = exeDir.parent_path() / "WIP";
			if (fs::exists(stdlibBase / "tokens"))
			{
				fileReader.addIncludePath(stdlibBase);
				fileReader.allowDirectory(stdlibBase);
			}
		}
	}
	catch (...) { /* best-effort, never fatal */ }

	// User-specified import paths.
	for (auto const& ip: _opts.importPaths)
	{
		auto absIp = solidity::frontend::FileReader::normalizeCLIPathForVFS(ip);
		if (absIp != fileReader.basePath()
			&& std::find(fileReader.includePaths().begin(),
				fileReader.includePaths().end(), absIp) == fileReader.includePaths().end())
			fileReader.addIncludePath(absIp);
		fileReader.allowDirectory(absIp);
	}

	return fileReader;
}

std::optional<std::string> readSourceFile(std::string const& _path)
{
	boost::system::error_code error;
	if (!fs::is_regular_file(_path, error)) return std::nullopt;
	std::ifstream file(_path, std::ios::binary);
	if (!file.is_open()) return std::nullopt;
	std::ostringstream ss;
	ss << file.rdbuf();
	if (file.bad() || ss.bad()) return std::nullopt;
	return ss.str();
}

void applyRemappings(
	solidity::frontend::CompilerStack& _compiler,
	solidity::frontend::FileReader& _fileReader,
	std::vector<std::string> const& _remappings)
{
	if (_remappings.empty())
		return;

	auto& logger = puyasol::Logger::instance();
	std::vector<solidity::frontend::ImportRemapper::Remapping> parsedRemappings;
	for (auto const& remapStr: _remappings)
	{
		auto parsed = solidity::frontend::ImportRemapper::parseRemapping(remapStr);
		if (parsed.has_value())
		{
			parsedRemappings.push_back(parsed.value());
			logger.debug("Remapping: '" + parsed->prefix + "' => '" + parsed->target + "'");
			// Allow remapping target dir on FileReader.
			fs::path targetPath(parsed->target);
			if (targetPath.is_absolute() && fs::exists(targetPath))
			{
				_fileReader.allowDirectory(targetPath);
				_fileReader.addIncludePath(targetPath);
			}
		}
		else
			logger.warning("Invalid remapping format: " + remapStr);
	}
	_compiler.setRemappings(parsedRemappings);
}

} // namespace

std::optional<CompilerSettings> resolveCompilerSettings(Options const& opts)
{
	using solidity::langutil::EVMVersion;
	auto version = opts.evmVersion.empty()
		? std::optional<EVMVersion>{EVMVersion::cancun()}
		: EVMVersion::fromString(opts.evmVersion);
	if (!version)
	{
		Logger::instance().error("Unknown EVM version '" + opts.evmVersion + "' (expected a solc-supported name)");
		return std::nullopt;
	}
	CompilerSettings settings{*version, {
		.evmStorageLayout = opts.evmStorageLayout,
		.evmSelectors = opts.evmSelectors || opts.contractAbi == "evm",
		.contractAbi = opts.contractAbi == "evm" ? builder::ContractAbi::Evm : builder::ContractAbi::Arc4,
		.viaIRSequencing = opts.viaYulBehavior,
		.proxyAdaptation = opts.proxyAdaptation,
		.evmChainId = opts.evmChainId.empty() ? std::nullopt : std::optional{opts.evmChainId},
		.evmBlockGasLimit = opts.evmBlockGasLimit.empty() ? std::nullopt : std::optional{opts.evmBlockGasLimit},
		.evmCoinbase = opts.evmCoinbase.empty() ? std::nullopt : std::optional{opts.evmCoinbase},
		.allowedEvmDivergences = opts.allowedEvmDivergences,
		.childProgramsViaBox = opts.childProgramsViaBox,
		.evmVersionName = version->name(),
		.scratchLayout = builder::ScratchLayout(opts.evmMemorySlots > 0
			? opts.evmMemorySlots : builder::ScratchLayout::defaultMemorySlots),
	}};
	if (!opts.xchainTemplate.empty())
	{
		if (settings.target.contractAbi != builder::ContractAbi::Evm)
		{
			Logger::instance().error("--xchain-template requires --contract-abi evm (the xchain account model lives in the 160-bit namespace)");
			return std::nullopt;
		}
		auto const& bytes = opts.xchainTemplate;
		auto const& placeholder = opts.xchainPlaceholder;
		auto match = std::search(bytes.begin(), bytes.end(), placeholder.begin(), placeholder.end());
		if (placeholder.size() != 20 || match == bytes.end()
			|| std::search(match + 1, bytes.end(), placeholder.begin(), placeholder.end()) != bytes.end())
		{
			Logger::instance().error("--xchain-template must contain the owner placeholder exactly once");
			return std::nullopt;
		}
		settings.target.xchainAccounts = builder::TargetProfile::XchainAccounts{
			{bytes.begin(), match}, {match + 20, bytes.end()}};
	}
	return settings;
}

SourceInput::SourceInput(Options const& options):
	m_options(options), m_mainPath(fs::absolute(options.sourceFiles.at(0))),
	m_reader(setupFileReader(options, m_mainPath))
{}

solidity::frontend::ReadCallback::Callback SourceInput::reader()
{
	return [this, base = m_reader.reader()](std::string const& kind, std::string const& path) {
		auto result = base(kind, path);
		if (result.success && m_options.legacySourceRewrite)
		{
			auto original = std::move(result.responseOrErrorMessage);
			result.responseOrErrorMessage = transformSource(original);
			m_rewrites[path] = {std::move(original), result.responseOrErrorMessage};
		}
		return result;
	};
}

bool SourceInput::load(solidity::frontend::CompilerStack& compiler)
{
	auto& logger = Logger::instance();
	// Remapping include paths must be finalized before naming any explicit file.
	applyRemappings(compiler, m_reader, m_options.remappings);
	solidity::frontend::FileReader::FileSystemPathSet paths;
	for (auto const& path: m_options.sourceFiles) paths.insert(fs::absolute(path));
	auto collisions = m_reader.detectSourceUnitNameCollisions(paths);
	for (auto const& [unit, files]: collisions)
	{
		std::string message = "Source unit name collision detected: " + unit + " matches";
		for (auto const& file: files) message += " '" + file.string() + "'";
		logger.error(message);
	}
	if (!collisions.empty()) return false;
	if (m_options.legacySourceRewrite)
		std::cerr << "WARNING: UNSAFE LEGACY SOURCE REWRITE ENABLED. Solidity source will be modified before parsing; "
			"exact before/after text and hashes will be written to source-rewrite-manifest.json.\n";
	for (size_t i = 0; i < m_options.sourceFiles.size(); ++i)
	{
		auto path = fs::absolute(m_options.sourceFiles[i]);
		auto unit = m_reader.cliPathToSourceUnitName(path);
		m_explicitAliases[path.string()] = unit;
		// Repeated spellings of the same normalized file are one source, including
		// the main-only legacy transformation. Different files were rejected above.
		if (m_reader.sourceUnits().contains(unit)) continue;
		auto contents = readSourceFile(path.string());
		if (!contents)
		{
			logger.error("Cannot read source file: " + path.string());
			return false;
		}
		if (m_options.legacySourceRewrite)
		{
			auto original = std::move(*contents);
			*contents = transformSource(original);
			if (i == 0)
				*contents = removeInheritedEvents(*contents,
					collectInterfaceEventsFromImports(original, m_mainPath.parent_path()));
			m_rewrites[unit] = {std::move(original), *contents};
		}
		m_reader.addOrUpdateFile(path, std::move(*contents));
		logger.info(i == 0 ? "Source: " + path.string() : "Additional source: " + unit);
	}
	compiler.setSources(m_reader.sourceUnits());
	return true;
}

std::map<std::string, std::string> SourceInput::aliases() const
{
	auto result = sourceFileAliases(m_reader);
	for (auto const& [path, unit]: m_explicitAliases) result[path] = unit;
	return result;
}

void reportCompilerDiagnostics(solidity::frontend::CompilerStack const& _compiler)
{
	auto& logger = puyasol::Logger::instance();
	for (auto const& error: _compiler.errors())
	{
		auto message = solidity::langutil::SourceReferenceFormatter::formatErrorInformation(
			*error, _compiler, false, true);
		while (!message.empty() && message.back() == '\n') message.pop_back();
		using Severity = solidity::langutil::Error::Severity;
		switch (error->severity())
		{
		case Severity::Info: logger.info(message); break;
		case Severity::Warning: logger.warning(message); break;
		case Severity::Error: logger.error(message); break;
		}
	}
}

} // namespace puyasol::cli
