#include "runner/PuyaRunner.h"
#include "Logger.h"

#include <cstdlib>
#include <sstream>

namespace puyasol::runner
{

int PuyaRunner::run(
	std::string const& _awstPath,
	std::string const& _optionsPath,
	std::string const& _logLevel
)
{
	if (m_puyaPath.empty())
	{
		Logger::instance().error("puya path not set");
		return 1;
	}

	std::ostringstream cmd;
	cmd << m_puyaPath
		<< " --awst " << _awstPath
		<< " --options " << _optionsPath
		<< " --log-level " << _logLevel;

	std::string cmdStr = cmd.str();
	Logger::instance().debug("Running: " + cmdStr);

	int result = std::system(cmdStr.c_str());

	if (result != 0)
		Logger::instance().error("puya exited with code: " + std::to_string(result));
	else
		Logger::instance().info("puya completed successfully");

	return result;
}

} // namespace puyasol::runner
