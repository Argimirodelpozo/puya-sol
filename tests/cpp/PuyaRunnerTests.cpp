#include "runner/PuyaRunner.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <unistd.h>

namespace fs = std::filesystem;

int main()
{
	auto const tempDir = fs::temp_directory_path()
		/ ("puya-sol runner test " + std::to_string(::getpid()));
	std::error_code ec;
	fs::remove_all(tempDir, ec);
	if (!fs::create_directories(tempDir, ec) || ec)
	{
		std::cerr << "failed to create test directory: " << ec.message() << '\n';
		return 1;
	}

	auto const backend = tempDir / "puya backend";
	fs::create_symlink("/bin/true", backend, ec);
	if (ec)
	{
		std::cerr << "failed to create backend symlink: " << ec.message() << '\n';
		fs::remove_all(tempDir, ec);
		return 1;
	}

	puyasol::runner::PuyaRunner runner;
	runner.setPuyaPath(backend.string());
	int const result = runner.run(
		(tempDir / "awst path;with shell chars.json").string(),
		(tempDir / "options path with spaces.json").string(),
		"info");

	fs::remove_all(tempDir, ec);
	if (result != 0)
	{
		std::cerr << "backend path/arguments were not passed literally; exit "
			<< result << '\n';
		return 1;
	}
	return 0;
}
