#include "runner/PuyaRunner.h"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;
using Runner = puyasol::runner::PuyaRunner;
using Status = Runner::Result::Status;
using namespace std::chrono_literals;

int main(int argc, char** argv)
{
	// This executable doubles as a controlled backend; no shell/Python needed.
	if (argc > 1)
	{
		if (argc != 7 || std::string(argv[1]) != "--awst"
			|| std::string(argv[3]) != "--options" || std::string(argv[5]) != "--log-level")
			return 90;
		std::string mode = argv[2];
		if (mode == "exit") return 17;
		if (mode == "127") return 127;
		if (mode == "signal") { ::raise(SIGTERM); return 91; }
		if (mode == "wait") { for (;;) ::pause(); }
		return mode == "awst path;literal.json"
			&& std::string(argv[4]) == "options path with spaces.json"
			&& std::string(argv[6]) == "debug with spaces" ? 0 : 92;
	}
	std::string pattern = (fs::temp_directory_path() / "puya runner.XXXXXX").string();
	if (!::mkdtemp(pattern.data())) return 1;
	fs::path directory(pattern);
	struct Cleanup { fs::path path; ~Cleanup() { fs::remove_all(path); } } cleanup{directory};
	auto backend = directory / "backend with spaces";
	fs::create_symlink(fs::canonical("/proc/self/exe"), backend);
	Runner runner(backend.string());
	int failures = 0;
	auto check = [&](bool ok, char const* message) {
		if (!ok) { std::cerr << message << '\n'; ++failures; }
	};
	check(runner.run("awst path;literal.json", "options path with spaces.json",
		"debug with spaces").exitCode() == 0, "exact argv/spaced executable");
	auto exited = runner.run("exit", "unused");
	check(exited.status == Status::Exited && exited.detail == 17, "nonzero backend exit");
	exited = runner.run("127", "unused");
	check(exited.status == Status::Exited && exited.detail == 127, "backend exit 127");
	auto signalled = runner.run("signal", "unused");
	check(signalled.status == Status::Signalled && signalled.exitCode() == 128 + SIGTERM,
		"signal propagation");
	auto missing = Runner((directory / "missing").string()).run("", "");
	check(missing.status == Status::LaunchFailed && missing.detail == ENOENT, "missing executable");
	auto nonexec = directory / "not executable";
	std::ofstream(nonexec) << "not an executable\n";
	auto denied = Runner(nonexec.string()).run("", "");
	check(denied.status == Status::LaunchFailed && denied.detail == EACCES, "execute permission");
	auto timed = runner.run("wait", "unused", "info",
		{std::chrono::steady_clock::now() + 50ms, {}});
	check(timed.status == Status::TimedOut, "deadline");
	std::stop_source cancellation;
	std::jthread cancel([&] { std::this_thread::sleep_for(50ms); cancellation.request_stop(); });
	auto cancelled = runner.run("wait", "unused", "info", {{}, cancellation.get_token()});
	check(cancelled.status == Status::Cancelled, "cancellation");
	check(runner.run("wait", "unused", "info", {{}, cancellation.get_token()}).status
		== Status::Cancelled, "already cancelled");
	int status;
	check(::waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD, "all owned children reaped");
	return failures ? 1 : 0;
}
