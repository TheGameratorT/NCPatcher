// Child-process working-directory and environment tests.
//
// The second case is the one with teeth on Windows. There every API has a
// narrow and a wide half, and the narrow half speaks the machine's ANSI code
// page: a user called Jos\u00e9 has a home directory that CreateProcessA cannot
// spell, so a build under it would fail with a path nobody could find in the
// error. The child is asked to report the directory it landed in rather than to
// echo anything, because a console echoes through the console code page, which
// mangles these characters even when they arrived intact.

#include "../source/system/process.hpp"
#include "../source/utils/unicode.hpp"

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
		// The wide half of the pair on Windows. The environment is delivered as
		// UTF-16, so narrow getenv would hand back the ANSI transcription of it
		// and this would be measuring the CRT rather than the delivery. A hook
		// that reads its environment narrowly on Windows has the same problem,
		// and it is the hook's to solve. What is being checked here is that
		// the value arrives whole.
		const auto variable = [](const char* name) -> std::string {
#ifdef _WIN32
			const std::wstring wideName = ncp::toWide(name);
			const wchar_t* value = _wgetenv(wideName.c_str());
			return value != nullptr ? ncp::toUtf8(value) : std::string();
#else
			const char* value = std::getenv(name);
			return value != nullptr ? std::string(value) : std::string();
#endif
		};

		std::cout << variable("NCP_HOOK_VALUE") << '|'
		          << ncp::pathToUtf8Generic(fs::current_path()) << '|'
		          << variable("NCP_PROCESS_INHERITED");
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
	const std::string command = '"' + ncp::pathToUtf8(executable) + "\" --child";

	int failures = 0;

	const auto run = [&](const char* what, const fs::path& where, const std::string& value) {
		const Process::Environment environment = { { "NCP_HOOK_VALUE", value } };
		std::ostringstream output;
		const int result = Process::start(command.c_str(), where, environment, &output);

		const std::string expected =
			value + "|" + ncp::pathToUtf8Generic(fs::canonical(where)) + "|kept";
		if (result != 0 || output.str() != expected)
		{
			std::cout << "FAIL: " << what << "\n"
			          << "  exit:     " << result << "\n"
			          << "  expected: " << expected << "\n"
			          << "  actual:   " << output.str() << "\n";
			failures++;
		}
	};

	run("child process context", cwd, "override");

	// "Jos\u00e9-\u6587\u5b57": one character no OEM code page spells correctly, and two
	// that no single-byte code page can hold at all. The working directory and
	// the environment value travel by different routes, so both carry it.
	{
		const std::string tricky = "Jos\xc3\xa9-\xe6\x96\x87\xe5\xad\x97";
		const fs::path awkward = root / ncp::utf8ToPath(tricky);
		fs::create_directories(awkward);
		run("a non-ASCII working directory and environment value", awkward, tricky);
	}

	fs::remove_all(root, ignored);
	if (failures == 0)
		std::cout << "process_test: all checks passed\n";
	return failures == 0 ? 0 : 1;
}
