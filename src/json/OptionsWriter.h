#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace puyasol::json
{

/// Generates the puya options.json file.
class OptionsWriter
{
public:
	/// Write options.json to the given path.
	static void write(
		std::string const& _path,
		std::string const& _contractName,
		std::string const& _outputDir
	);
};

} // namespace puyasol::json
