#pragma once

#include <chrono>
#include <csignal>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>

namespace puyasol::runner
{

/// Invokes the puya CLI backend to compile AWST JSON to TEAL.
class PuyaRunner
{
public:
	explicit PuyaRunner(std::string path): m_puyaPath(std::move(path)) {}

	struct Control
	{
		std::optional<std::chrono::steady_clock::time_point> deadline;
		std::stop_token cancellation;
		/// Optional signal-handler flag owned by the caller; no handlers are installed here.
		volatile std::sig_atomic_t const* signal = nullptr;
	};
	struct Result
	{
		enum class Status { Exited, Signalled, LaunchFailed, WaitFailed, TimedOut, Cancelled };
		Status status;
		/// Exit code, signal, or errno, according to status.
		int detail = 0;
		int exitCode() const;
	};

	/// Run puya with the given awst.json and options.json.
	/// No implicit deadline. Cancellation/deadline kill and reap this invocation's
	/// process group only; stdout/stderr and PATH lookup retain normal CLI behavior.
	Result run(
		std::string const& _awstPath,
		std::string const& _optionsPath,
		std::string const& _logLevel = "info",
		Control const& _control = {{}, {}, nullptr}
	) const;

private:
	std::string m_puyaPath;
};

} // namespace puyasol::runner
