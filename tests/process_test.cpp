// Child-process working-directory and environment tests.

#include "../source/system/process.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

int main(int argc, char* argv[])
{
	if (argc == 2 && std::string(argv[1]) == "--child")
	{
		const char* value = std::getenv("NCP_HOOK_VALUE");
		const char* inherited = std::getenv("NCP_PROCESS_INHERITED");
		std::cout << (value ? value : "") << '|'
		          << fs::current_path().generic_string() << '|'
		          << (inherited ? inherited : "");
		return 0;
	}

	const fs::path root = fs::temp_directory_path() / "ncp_process_test";
	const fs::path cwd = root / "hook-cwd";
	std::error_code ignored;
	fs::remove_all(root, ignored);
	fs::create_directories(cwd);

#ifdef _WIN32
	_putenv_s("NCP_PROCESS_INHERITED", "kept");
#else
	setenv("NCP_PROCESS_INHERITED", "kept", 1);
#endif

	const fs::path executable = fs::absolute(argv[0]);
	const std::string command = '"' + executable.string() + "\" --child";
	const Process::Environment environment = { { "NCP_HOOK_VALUE", "override" } };
	std::ostringstream output;
	const int result = Process::start(command.c_str(), cwd, environment, &output);

	const std::string expected = "override|" + fs::canonical(cwd).generic_string() + "|kept";
	if (result != 0 || output.str() != expected)
	{
		std::cout << "FAIL: child process context\n"
		          << "  exit:     " << result << "\n"
		          << "  expected: " << expected << "\n"
		          << "  actual:   " << output.str() << "\n";
		return 1;
	}

	fs::remove_all(root, ignored);
	std::cout << "process_test: all checks passed\n";
	return 0;
}
