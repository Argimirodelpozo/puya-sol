#pragma once

#include "ArtifactIO.h"

#include <boost/filesystem/path.hpp>

#include <map>
#include <set>
#include <string>
#include <vector>

namespace puyasol::json
{

/// Generates the puya options.json file.
class OptionsWriter
{
public:
	/// Atomically write options.json for the complete compilation set.
	static bool write(
		boost::filesystem::path const& _path,
		std::vector<std::string> const& _contractNames,
		std::string const& _outputDir,
		int _optimizationLevel,
		bool _outputIr,
		std::set<std::string> const& _templateVarChildren,
		std::map<std::string, int64_t> const& _intTemplateVars,
		artifact::Digest& _digest,
		std::string& _error
	);
};

} // namespace puyasol::json
