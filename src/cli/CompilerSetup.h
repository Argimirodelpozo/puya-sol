/// @file CompilerSetup.h
/// CompilerStack environment plumbing moved out of main.cpp: FileReader
/// allowlist/include-path setup, EVM-version name resolution, Foundry-style
/// remapping application, and the 0.5.x-compat error-leniency filter.
#pragma once

#include "cli/CliOptions.h"

#include <libsolidity/interface/CompilerStack.h>
#include <libsolidity/interface/FileReader.h>
#include <liblangutil/EVMVersion.h>

#include <boost/filesystem.hpp>

#include <optional>
#include <string>

namespace puyasol::cli
{

/// Set up the Solidity FileReader with allowed directories and include
/// paths: source dir, project root, optional node_modules, the puya-sol
/// stdlib at `<exe>/../src` + `<exe>/../WIP/`, and any user-specified
/// --import-path entries.
solidity::frontend::FileReader setupFileReader(
	Options const& _opts,
	boost::filesystem::path const& _sourceDir,
	boost::filesystem::path const& _projectRoot);

/// Read `_path` into a string. Returns std::nullopt if the file can't be opened.
std::optional<std::string> readSourceFile(std::string const& _path);

/// Resolve `--evm-version <name>` to a concrete EVMVersion. Empty or unknown
/// names yield the default (cancun); unknown names additionally warn.
solidity::langutil::EVMVersion resolveEvmVersion(std::string const& _name);

/// Apply Foundry-style import remappings (prefix=target) to the compiler and
/// allow their target directories on the FileReader.
void applyRemappings(
	solidity::frontend::CompilerStack& _compiler,
	solidity::frontend::FileReader& _fileReader,
	std::vector<std::string> const& _remappings);

/// Walk the compiler's errors after a failed parseAndAnalyze, suppressing the
/// 0.5.x→0.8.x compat classes (duplicate interface events, implicit diamond
/// override) and logging the rest. Returns true when only suppressed/warning
/// diagnostics remain — i.e. compilation may proceed to the AST.
bool reportCompilationErrors(solidity::frontend::CompilerStack const& _compiler);

} // namespace puyasol::cli
