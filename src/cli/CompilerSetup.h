#pragma once

#include "builder/TargetProfile.h"
#include "cli/CliOptions.h"
#include "cli/SourceCompat.h"

#include <libsolidity/interface/CompilerStack.h>
#include <libsolidity/interface/FileReader.h>
#include <liblangutil/EVMVersion.h>

#include <optional>
#include <string>

namespace puyasol::cli
{

struct CompilerSettings
{
	solidity::langutil::EVMVersion evmVersion;
	builder::TargetProfile target;
};

/// Resolve option-only policy before opening files. Invalid options are logged.
std::optional<CompilerSettings> resolveCompilerSettings(Options const& options);

/// Own the VFS and callback state for one compilation. Must outlive CompilerStack.
class SourceInput
{
public:
	explicit SourceInput(Options const& options);
	SourceInput(SourceInput const&) = delete;
	SourceInput& operator=(SourceInput const&) = delete;

	solidity::frontend::ReadCallback::Callback reader();
	bool load(solidity::frontend::CompilerStack& compiler);
	std::map<std::string, std::string> aliases() const;
	std::string mainFile() const { return m_mainPath.string(); }
	SourceRewriteMap const& rewrites() const { return m_rewrites; }

private:
	Options const& m_options;
	boost::filesystem::path m_mainPath;
	solidity::frontend::FileReader m_reader;
	SourceRewriteMap m_rewrites;
	std::map<std::string, std::string> m_explicitAliases;
};

/// Preserve solc's severity, diagnostic IDs and primary/secondary source ranges.
void reportCompilerDiagnostics(solidity::frontend::CompilerStack const& compiler);

} // namespace puyasol::cli
