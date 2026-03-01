#pragma once

#include <nlohmann/json.hpp>
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
		int _optimizationLevel = 1
	);

	/// Write options.json for multiple contracts (split contract mode).
	static void writeMultiple(
		std::string const& _path,
		std::vector<std::string> const& _contractNames,
		std::string const& _outputDir,
		int _optimizationLevel = 1
	);
};

} // namespace puyasol::json
