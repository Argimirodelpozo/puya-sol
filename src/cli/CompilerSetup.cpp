#include "cli/CompilerSetup.h"
#include "Logger.h"

#include <libsolidity/interface/ImportRemapper.h>

#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace fs = boost::filesystem;

namespace puyasol::cli
{

solidity::frontend::FileReader setupFileReader(
	Options const& _opts,
	fs::path const& _sourceDir,
	fs::path const& _projectRoot)
{
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

	// Resolve stdlib roots via /proc/self/exe (no-op on non-procfs platforms):
	//   <root>/src/  — bundled libraries (`import "libs/AVM.sol"`)
	//   <root>/WIP/  — example contracts (`import "tokens/AERC20.sol"`)
	try
	{
		char execPathBuf[4096];
		ssize_t len = ::readlink("/proc/self/exe", execPathBuf, sizeof(execPathBuf) - 1);
		if (len > 0)
		{
			execPathBuf[len] = '\0';
			fs::path root = fs::path(execPathBuf).parent_path().parent_path();
			// Bundled libraries (libs/AVM.sol etc.)
			fs::path libsBase = root / "src";
			if (fs::exists(libsBase / "libs"))
			{
				fileReader.addIncludePath(libsBase);
				fileReader.allowDirectory(libsBase);
			}
			// Example contracts (tokens/, examples/)
			fs::path stdlibBase = root / "WIP";
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
		fs::path absIp = fs::absolute(ip);
		fileReader.addIncludePath(absIp);
		fileReader.allowDirectory(absIp);
	}

	return fileReader;
}

std::optional<std::string> readSourceFile(std::string const& _path)
{
	std::ifstream file(_path);
	if (!file.is_open()) return std::nullopt;
	std::ostringstream ss;
	ss << file.rdbuf();
	return ss.str();
}

solidity::langutil::EVMVersion resolveEvmVersion(std::string const& _name)
{
	// Default: cancun. Test runner translates `// EVMVersion: ...` directives
	// to --evm-version. Accepts any solc-supported name (homestead..osaka).
	auto const defaultVersion = solidity::langutil::EVMVersion::cancun();
	if (_name.empty())
		return defaultVersion;

	// Keep accepted version names in lockstep with the vendored compiler instead
	// of duplicating solc's version table here (and drifting on future forks).
	if (auto evmVer = solidity::langutil::EVMVersion::fromString(_name))
		return *evmVer;

	// FATAL: a misspelled target would silently compile as cancun with
	// different accepted syntax and opcode gating.
	puyasol::Logger::instance().error(
		"Unknown EVM version '" + _name + "' (accepted: solc names homestead..osaka)");
	std::exit(2);
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

bool reportCompilationErrors(solidity::frontend::CompilerStack const& _compiler)
{
	auto& logger = puyasol::Logger::instance();

	bool hasError = false;
	for (auto const& error: _compiler.errors())
	{
		if (error->type() == solidity::langutil::Error::Type::Warning)
			continue;

		std::string msg = error->what();

		// Include source location in the error message.
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
	return !hasError;
}

} // namespace puyasol::cli
