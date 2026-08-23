#pragma once

#include <filesystem>
#include <ostream>

namespace Process
{
	// Runs `cmd` through the system shell, streaming its merged stdout/stderr to
	// `out` when given, and returns its exit code.
	//
	// `cwd` is the working directory of the child only -- the calling process
	// never changes its own. Pass an empty path to inherit ours. This exists
	// because compiler command lines carry relative paths that must resolve
	// against the target's directory, which used to be arranged by chdir'ing the
	// whole process around the build.
	int start(const char* cmd, const std::filesystem::path& cwd, std::ostream* out = nullptr);

	int start(const char* cmd, std::ostream* out = nullptr);

	bool exists(const char* app);
}
