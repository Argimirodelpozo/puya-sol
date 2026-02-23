#pragma once

#include <string>

namespace puyasol::runner
{

/// Invokes the puya CLI backend to compile AWST JSON to TEAL.
class PuyaRunner
{
public:
	/// Set the path to the puya executable.
	void setPuyaPath(std::string const& _path) { m_puyaPath = _path; }

	/// Run puya with the given awst.json and options.json.
	/// Returns the exit code.
	int run(
		std::string const& _awstPath,
		std::string const& _optionsPath,
		std::string const& _logLevel = "info"
	);

private:
	std::string m_puyaPath;
};

} // namespace puyasol::runner
