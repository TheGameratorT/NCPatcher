#pragma once

// What the invocation asked for, before any of it has been checked against a
// project.
//
// This is deliberately a plain struct with no behaviour: parsing and acting are
// separate so that `config dump` can report where every setting came from, and
// so that the precedence rule -- command line, then NCPATCHER_* environment,
// then the project file, then the built-in default -- is applied in one visible
// place rather than being an emergent property of the order things were read.

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "context.hpp"
#include "../config/project_config.hpp"
#include "../system/log.hpp"
#include "../system/message.hpp"

namespace ncp {

enum class Command
{
	Build,
	Clean,
	Restore,
	ConfigDump,
	ConfigValidate,
	ConfigPath,
	Migrate,
	Version
};

struct CommandLine
{
	Command command = Command::Build;

	// -C/--project. Either the project directory or the configuration file
	// itself; the difference is decided by looking at the path, so that both
	// `-C mymod` and `-C mymod/ncpatcher.yaml` do what they look like.
	std::filesystem::path projectPath;

	// --rom, overriding the configured extracted-ROM directory. This is what
	// removes the "launch it from the right working directory" constraint that
	// the level editor and CTGPNitro's build script both work around.
	std::filesystem::path romDir;

	std::vector<std::string> defines;   // -D/--define
	std::vector<std::string> vars;      // --var NAME=VALUE

	std::string toolchain;              // --toolchain / NCPATCHER_TOOLCHAIN
	config::Source toolchainSource = config::Source::Default;

	int jobs = 0;                       // -j/--jobs / NCPATCHER_JOBS
	config::Source jobsSource = config::Source::Default;

	std::unordered_set<VerboseTag> verboseTags;

	Log::ColorMode color = Log::ColorMode::Auto;
	msg::Format messageFormat = msg::Format::Human;
	std::filesystem::path resultPath;   // --result

	std::filesystem::path logPath;      // --log / NCPATCHER_LOG
	bool logPathSet = false;
	bool noLog = false;                 // --no-log

	// clean
	bool cleanBackups = false;
	// migrate
	bool migrateWrite = false;
	// config dump
	bool explain = false;
	bool dumpJson = false;
};

// Parses argv, then folds in the NCPATCHER_* environment for the settings that
// have one.
//
// Returns an exit code when the process should stop right away -- because --help
// or --version was asked for, or because the command line did not parse -- and
// nothing when the parsed result in `out` should be acted on.
[[nodiscard]] std::optional<int> parseCommandLine(int argc, char* argv[], CommandLine& out);

// "1.0.4". Shared by `version`, --version and the runtime header stamp.
[[nodiscard]] const char* versionString();

} // namespace ncp
