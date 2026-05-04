#pragma once

#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <vector>

namespace puyasol::json
{

/// Generates the puya options.json file.
class OptionsWriter
{
public:
	/// Write options.json to the given path (single contract).
	static void write(
		std::string const& _path,
		std::string const& _contractName,
		std::string const& _outputDir,
		int _optimizationLevel = 1,
		bool _outputIr = false,
		std::set<std::string> const& _templateVarChildren = {},
		std::map<std::string, int64_t> const& _intTemplateVars = {}
	);

	/// Write options.json for multiple contracts (split contract mode).
	static void writeMultiple(
		std::string const& _path,
		std::vector<std::string> const& _contractNames,
		std::string const& _outputDir,
		int _optimizationLevel = 1,
		bool _outputIr = false,
		std::set<std::string> const& _templateVarChildren = {},
		std::map<std::string, int64_t> const& _intTemplateVars = {}
	);
};

} // namespace puyasol::json
