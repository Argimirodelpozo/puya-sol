/// @file CompilerSetup.h
/// CompilerStack environment plumbing moved out of main.cpp: FileReader
/// allowlist/include-path setup, EVM-version name resolution, Foundry-style
/// remapping application, and compiler diagnostic reporting.
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
/// stdlib in the build/install `share/puya-sol` layout (plus source-tree
/// fallbacks), and any user-specified --import-path entries.
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

/// Report every non-warning diagnostic after parseAndAnalyze. Returns true
/// only when no errors remain; builder must never consume a partially analyzed
/// solc AST.
bool reportCompilationErrors(solidity::frontend::CompilerStack const& _compiler);

} // namespace puyasol::cli
