#include "runner/PuyaRunner.h"
#include "Logger.h"

#include <cerrno>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>

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

	Logger::instance().debug(
		"Running puya backend: " + m_puyaPath
		+ " --awst " + _awstPath
		+ " --options " + _optionsPath
		+ " --log-level " + _logLevel);

	// Do not route compiler-controlled paths through `/bin/sh -c`. Apart from
	// breaking ordinary paths containing spaces, system() made --puya-path and
	// --output-dir shell-injection surfaces. Pass each argument directly to exec.
	pid_t child = ::fork();
	if (child < 0)
	{
		Logger::instance().error(
			"failed to fork puya backend: " + std::string(std::strerror(errno)));
		return 1;
	}
	if (child == 0)
	{
		::execlp(
			m_puyaPath.c_str(),
			m_puyaPath.c_str(),
			"--awst", _awstPath.c_str(),
			"--options", _optionsPath.c_str(),
			"--log-level", _logLevel.c_str(),
			static_cast<char*>(nullptr));
		// Avoid touching parent-process buffered streams after fork.
		::_exit(127);
	}

	int status = 0;
	while (::waitpid(child, &status, 0) < 0)
	{
		if (errno == EINTR)
			continue;
		Logger::instance().error(
			"failed waiting for puya backend: " + std::string(std::strerror(errno)));
		return 1;
	}

	int result = 1;
	if (WIFEXITED(status))
		result = WEXITSTATUS(status);
	else if (WIFSIGNALED(status))
		result = 128 + WTERMSIG(status);

	if (result != 0)
		Logger::instance().error("puya exited with code: " + std::to_string(result));
	else
		Logger::instance().debug("puya backend process exited with code 0");

	return result;
}

} // namespace puyasol::runner
