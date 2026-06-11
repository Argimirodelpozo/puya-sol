#include "cli/CompilerSetup.h"
#include "Logger.h"

#include <libsolidity/interface/ImportRemapper.h>

#include <unistd.h>

#include <fstream>
#include <map>
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

	// puya-sol stdlib, located relative to the executable (build/puya-sol →
	// repo root) and resolved via /proc/self/exe; a no-op on platforms without
	// procfs. Two bases:
	//   <root>/src/  — bundled Solidity libraries: `import "libs/AVM.sol"`.
	//   <root>/WIP/  — example contracts: `import "tokens/AERC20.sol"`.
	try
	{
		char execPathBuf[4096];
		ssize_t len = ::readlink("/proc/self/exe", execPathBuf, sizeof(execPathBuf) - 1);
		if (len > 0)
		{
			execPathBuf[len] = '\0';
			fs::path root = fs::path(execPathBuf).parent_path().parent_path();
			// Bundled libraries (libs/AVM.sol etc.).
			fs::path libsBase = root / "src";
			if (fs::exists(libsBase / "libs"))
			{
				fileReader.addIncludePath(libsBase);
				fileReader.allowDirectory(libsBase);
			}
			// Example contracts (tokens/, examples/).
			fs::path stdlibBase = root / "WIP";
			if (fs::exists(stdlibBase / "tokens"))
			{
				fileReader.addIncludePath(stdlibBase);
				fileReader.allowDirectory(stdlibBase);
			}
		}
	}
	catch (...) { /* best-effort, never fatal */ }

	// Allow user-specified import paths
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
	// Default is cancun; `--evm-version <name>` overrides — accepts any
	// solc-supported name (homestead..osaka). The test runner translates
	// fixture-side directives (`// EVMVersion: ...`) to a concrete name and
	// passes the flag.
	auto evmVer = solidity::langutil::EVMVersion::cancun();
	if (_name.empty())
		return evmVer;

	using V = solidity::langutil::EVMVersion;
	static std::map<std::string, V> const namedVersions = {
		{"homestead",        V::homestead()},
		{"tangerineWhistle", V::tangerineWhistle()},
		{"spuriousDragon",   V::spuriousDragon()},
		{"byzantium",        V::byzantium()},
		{"constantinople",   V::constantinople()},
		{"petersburg",       V::petersburg()},
		{"istanbul",         V::istanbul()},
		{"berlin",           V::berlin()},
		{"london",           V::london()},
		{"paris",            V::paris()},
		{"shanghai",         V::shanghai()},
		{"cancun",           V::cancun()},
		{"prague",           V::prague()},
		{"osaka",            V::osaka()},
	};
	auto it = namedVersions.find(_name);
	if (it != namedVersions.end())
		evmVer = it->second;
	else
		puyasol::Logger::instance().warning(
			"Unknown EVM version '" + _name + "'; defaulting to cancun");
	return evmVer;
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
			// Allow the remapping target directory so FileReader can read from it
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

	// Check if we only have warnings (no errors)
	// Some errors from 0.5.x→0.8.x compat are treated as warnings
	bool hasError = false;
	for (auto const& error: _compiler.errors())
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
