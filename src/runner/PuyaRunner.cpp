#include "runner/PuyaRunner.h"
#include "Logger.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

namespace puyasol::runner
{

int PuyaRunner::Result::exitCode() const
{
	if (status == Status::Exited) return detail;
	if (status == Status::Signalled) return 128 + detail;
	return 1;
}

PuyaRunner::Result PuyaRunner::run(
	std::string const& awstPath, std::string const& optionsPath,
	std::string const& logLevel, Control const& control) const
{
	using Status = Result::Status;
	auto stopped = [&]() -> std::optional<Result> {
		if (control.signal && *control.signal) return Result{Status::Signalled, *control.signal};
		if (control.cancellation.stop_requested()) return Result{Status::Cancelled};
		if (control.deadline && std::chrono::steady_clock::now() >= *control.deadline)
			return Result{Status::TimedOut};
		return {};
	};
	if (auto reason = stopped()) return *reason;
	// posix_spawnp reports exec errors separately from a backend that exits 127.
	// No shell: paths and log levels remain literal arguments.
	char* const args[] = {
		const_cast<char*>(m_puyaPath.c_str()), const_cast<char*>("--awst"),
		const_cast<char*>(awstPath.c_str()), const_cast<char*>("--options"),
		const_cast<char*>(optionsPath.c_str()), const_cast<char*>("--log-level"),
		const_cast<char*>(logLevel.c_str()), nullptr};
	posix_spawnattr_t attributes;
	int error = ::posix_spawnattr_init(&attributes);
	pid_t child = -1;
	if (!error)
	{
		error = ::posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
		if (!error) error = ::posix_spawnattr_setpgroup(&attributes, 0);
		if (!error) error = ::posix_spawnp(&child, m_puyaPath.c_str(), nullptr,
			&attributes, args, environ);
		::posix_spawnattr_destroy(&attributes);
	}
	if (error)
	{
		Logger::instance().error("cannot launch puya backend: " + std::string(std::strerror(error)));
		return {Status::LaunchFailed, error};
	}

	int status = 0;
	for (;;)
	{
		bool const controlled = control.deadline || control.cancellation.stop_possible() || control.signal;
		auto waited = ::waitpid(child, &status, controlled ? WNOHANG : 0);
		if (waited == child) break;
		if (waited < 0)
		{
			if (errno == EINTR) continue;
			error = errno;
			Logger::instance().error("cannot wait for puya backend: " + std::string(std::strerror(error)));
			return {Status::WaitFailed, error};
		}
		if (auto reason = stopped())
		{
			// The child is not reaped yet, so its PID cannot be recycled. Its
			// dedicated process group includes subprocesses it may have started.
			::kill(-child, SIGKILL);
			while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
			Logger::instance().error(reason->status == Status::TimedOut
				? "puya backend deadline exceeded" : "puya backend cancelled");
			return *reason;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	Result result = WIFEXITED(status) ? Result{Status::Exited, WEXITSTATUS(status)}
		: Result{Status::Signalled, WTERMSIG(status)};
	if (result.exitCode())
		Logger::instance().error("puya exited with code: " + std::to_string(result.exitCode()));
	return result;
}

} // namespace puyasol::runner
