#include "cli.hpp"

#include <cstdlib>
#include <iostream>
#include <map>

#include "../../vendor/CLI11/CLI11.hpp"

#include "../system/exit_code.hpp"

namespace fs = std::filesystem;

namespace ncp {

#ifndef NCP_VERSION
#define NCP_VERSION "0.0.0-dev"
#endif

const char* versionString()
{
	return NCP_VERSION;
}

namespace {

const std::map<std::string, VerboseTag> VERBOSE_TAGS = {
	{ "build",   VerboseTag::Build },
	{ "section", VerboseTag::Section },
	{ "elf",     VerboseTag::Elf },
	{ "patch",   VerboseTag::Patch },
	{ "library", VerboseTag::Library },
	{ "linking", VerboseTag::Linking },
	{ "symbols", VerboseTag::Symbols },
	{ "nolib",   VerboseTag::NoLib },
	{ "all",     VerboseTag::All },
};

std::string verboseTagList()
{
	std::string out;
	for (const auto& [name, tag] : VERBOSE_TAGS)
	{
		(void)tag;
		if (!out.empty())
			out += ", ";
		out += name;
	}
	return out;
}

// getenv, but an empty value counts as unset. An exported-but-empty variable is
// almost always a script that meant to leave it alone.
std::optional<std::string> environment(const char* name)
{
	const char* value = std::getenv(name);
	if (value == nullptr || *value == '\0')
		return std::nullopt;
	return std::string(value);
}

void addGlobalOptions(CLI::App& app, CommandLine& out, std::vector<std::string>& verboseTagNames)
{
	app.add_option("-C,--project", out.projectPath,
		"Project directory, or the configuration file itself")
		->type_name("PATH");

	app.add_option("--rom", out.romPath,
		"ROM to patch (a .nds or an extracted directory), overriding the configuration")
		->type_name("PATH");

	app.add_option("--out", out.outPath,
		"Write the patched ROM here instead of patching it in place")
		->type_name("PATH");

	CLI::Option* variant = app.add_option("--variant", out.variant,
		"Build one named variant")
		->type_name("NAME");

	CLI::Option* allVariants = app.add_flag("--all-variants", out.allVariants,
		"Build every configured variant");

	variant->excludes(allVariants);
	allVariants->excludes(variant);

	app.add_option("-D,--define", out.defines,
		"Define a preprocessor macro, as NAME or NAME=VALUE")
		->type_name("NAME[=VAL]");

	app.add_option("--var", out.vars,
		"Set a configuration variable, overriding vars: in the project file")
		->type_name("NAME=VAL");

	app.add_option("--toolchain", out.toolchain,
		"Cross-compiler prefix, e.g. arm-none-eabi-")
		->type_name("PREFIX");

	app.add_option("-j,--jobs", out.jobs,
		"Number of compile jobs; 0 uses one per hardware thread")
		->option_text("N")
		->check(CLI::NonNegativeNumber);

	app.add_flag("-v,--verbose", "Enable every category of verbose output");

	app.add_option("--verbose-tag", verboseTagNames,
		"Enable one category of verbose output (" + verboseTagList() + ")")
		->type_name("TAG");

	app.add_option("--color", out.color, "When to style the console output")
		->transform(CLI::CheckedTransformer(std::map<std::string, Log::ColorMode>{
			{ "auto",   Log::ColorMode::Auto },
			{ "always", Log::ColorMode::Always },
			{ "never",  Log::ColorMode::Never },
		}, CLI::ignore_case))
		->option_text("auto|always|never [auto]");

	app.add_option("--message-format", out.messageFormat,
		"human prints a log; json emits one JSON object per line on stdout "
		"and moves the log to stderr")
		->transform(CLI::CheckedTransformer(std::map<std::string, msg::Format>{
			{ "human", msg::Format::Human },
			{ "json",  msg::Format::Json },
		}, CLI::ignore_case))
		->option_text("human|json [human]");

	app.add_option("--result", out.resultPath,
		"Write a JSON summary of the run to this file")
		->type_name("PATH");

	app.add_option("--log", out.logPath, "Write the log file here")
		->type_name("PATH");

	app.add_flag("--no-log", out.noLog, "Do not write a log file");
	app.add_flag("--no-env-file", out.noEnvFile,
	             "Ignore the project's .ncpatcher.env");
}

} // namespace

std::optional<int> parseCommandLine(int argc, char* argv[], CommandLine& out)
{
	CLI::App app{
		"NCPatcher compiles C, C++ and assembly and splices the result into a "
		"Nintendo DS ROM.",
		"ncpatcher"
	};

	app.set_help_flag("-h,--help", "Show this help message and exit");
	app.set_version_flag("--version", versionString(), "Show the version and exit");

	// A bare invocation is answered with a summary rather than a build. Starting
	// one is a decision with side effects (it writes into the ROM) and it
	// should be asked for by name.
	app.require_subcommand(0, 1);

	// Lets a global option written after the subcommand reach the parent.
	app.fallthrough();

	std::vector<std::string> verboseTagNames;
	addGlobalOptions(app, out, verboseTagNames);

	CLI::App* build = app.add_subcommand("build", "Compile and patch");

	CLI::App* init = app.add_subcommand("init", "Create a version 2 project in the project directory");
	init->add_option("--template", out.initTemplate,
		"Project template: default or nsmb")
		->type_name("NAME")
		->check(CLI::IsMember({ "default", "nsmb" }))
		->default_val("default");

	CLI::App* clean = app.add_subcommand("clean", "Delete the build directories");
	clean->add_flag("--backups", out.cleanBackups,
		"Also delete the backup directory. The ROM binaries stay patched and "
		"can no longer be restored; see 'restore' if that is not what you want.");

	CLI::App* restore = app.add_subcommand("restore",
		"Put the ROM binaries back the way they were and remove the backups");

	CLI::App* config = app.add_subcommand("config", "Inspect the resolved configuration");
	config->require_subcommand(1);

	CLI::App* configDump = config->add_subcommand("dump",
		"Print every setting after inheritance, variables and globs are resolved");
	configDump->add_flag("--explain", out.explain, "Say where each setting came from");
	configDump->add_flag("--json", out.dumpJson, "Print it as JSON");

	CLI::App* configValidate = config->add_subcommand("validate",
		"Load the configuration and report any problems, without building");
	CLI::App* configPath = config->add_subcommand("path",
		"Print the path of the configuration file that would be used");

	CLI::App* migrate = app.add_subcommand("migrate",
		"Convert a version 1 ncpatcher.json into ncpatcher.yaml");
	migrate->add_flag("--write", out.migrateWrite,
		"Save the result instead of printing it");

	CLI::App* modules = app.add_subcommand("modules",
		"Inspect the module graph the enabled modules add up to");
	modules->require_subcommand(1);

	CLI::App* modulesList = modules->add_subcommand("list",
		"Show which modules are enabled and what they contribute");

	// The dump is JSON and only JSON: it exists to be read by the generator that
	// turns a project's modules into game-specific headers, and a second format
	// would only be a second thing to keep in step.
	CLI::App* modulesDump = modules->add_subcommand("dump",
		"Print the resolved module graph as JSON");
	modulesDump->add_option("-o,--output", out.modulesOutPath,
		"Write it to this file instead of standard output")
		->type_name("PATH");

	CLI::App* modulesExplain = modules->add_subcommand("explain",
		"Say why one module or component is where it is");
	modulesExplain->add_option("name", out.modulesTarget,
		"A module, or a component as Module.Component")
		->required()->type_name("NAME");

	CLI::App* rom = app.add_subcommand("rom", "Inspect a ROM, or move its code binaries in and out");
	rom->require_subcommand(1);

	CLI::App* romInfo = rom->add_subcommand("info",
		"Print what the ROM header says about the ROM");

	CLI::App* romFiles = rom->add_subcommand("files",
		"List the ROM's NitroFS files, with their ids");
	romFiles->add_flag("--json", out.dumpJson, "Print it as ncpatcher.files/1 JSON");

	// The whole ROM, so that what comes out is a directory `rom: dir:` can be
	// pointed at and read back in full. An extraction that stopped at the code
	// binaries was only ever half of one, and every tool reading it had to know
	// which half.
	CLI::App* romExtract = rom->add_subcommand("extract",
		"Write the ROM into a directory the patcher can read back");
	romExtract->add_option("dir", out.romDirArgument, "Directory to write into")
		->required()->type_name("DIR");
	romExtract->add_flag("--code-only", out.extractCodeOnly,
		"Write only the code binaries, without the filesystem, banner or tables");
	romExtract->add_flag("--decompress-overlays", out.extractDecompressOverlays,
		"Write overlay bytes decompressed, clearing the compression flag in the "
		"emitted overlay table to match. `rom pack` restores the original form.");

	CLI::App* romPack = rom->add_subcommand("pack",
		"Fold a directory of code binaries back into a ROM");
	romPack->add_option("dir", out.romDirArgument, "Directory to read from")
		->required()->type_name("DIR");

	// Its own group rather than a `rom` subcommand: `rom files` reports a ROM
	// that exists, and this reports one that does not yet. Filing the two under
	// the same noun would suggest they answer the same question.
	CLI::App* files = app.add_subcommand("files",
		"Inspect the NitroFS files a build would place");
	files->require_subcommand(1);

	CLI::App* filesPlan = files->add_subcommand("plan",
		"Report what a build would place in the ROM's file table, without building");
	filesPlan->add_flag("--json", out.dumpJson, "Print it as ncpatcher.files/1 JSON");
	filesPlan->add_option("-o,--output", out.filesOutPath,
		"Write it to this file instead of standard output")
		->type_name("PATH");

	CLI::App* version = app.add_subcommand("version", "Show the version and exit");

	// Every subcommand accepts the global options too, so that both
	// `ncpatcher -v build` and `ncpatcher build -v` work.
	//
	// They do not appear in the subcommand's own --help, though, because CLI11
	// lists only what was added to that subcommand. Left alone, `build --help`
	// shows nothing but -h and reads as though the command took no options at
	// all, and `rom files --help` hides the one option that answers the
	// obvious question, which ROM. The footer says where they are.
	for (CLI::App* sub : { build, init, clean, restore, configDump, configValidate, configPath, migrate,
	                       modulesList, modulesDump, modulesExplain,
	                       romInfo, romFiles, romExtract, romPack, filesPlan })
	{
		sub->fallthrough();
		sub->footer("The global options are accepted here too, before or after the command;\n"
		            "`ncpatcher --help` lists them. --rom chooses which ROM to act on.");
	}

	try {
		app.parse(argc, argv);
	} catch (const CLI::ParseError& e) {
		const int code = app.exit(e);
		// --help and --version leave through here with 0; anything else is the
		// command line being wrong, which is a usage error whatever CLI11 calls it.
		return code == 0 ? 0 : exitValue(ExitCode::Usage);
	}

	if (app.count("--verbose") > 0)
		out.verboseTags.insert(VerboseTag::All);

	for (const std::string& name : verboseTagNames)
	{
		const auto it = VERBOSE_TAGS.find(name);
		if (it == VERBOSE_TAGS.end())
		{
			std::cerr << "Unknown verbose tag: " << name << "\n"
			          << "Expected one of: " << verboseTagList() << std::endl;
			return exitValue(ExitCode::Usage);
		}
		out.verboseTags.insert(it->second);
	}

	if (*init)               out.command = Command::Init;
	else if (*clean)         out.command = Command::Clean;
	else if (*restore)       out.command = Command::Restore;
	else if (*configDump)    out.command = Command::ConfigDump;
	else if (*configValidate) out.command = Command::ConfigValidate;
	else if (*configPath)    out.command = Command::ConfigPath;
	else if (*migrate)       out.command = Command::Migrate;
	else if (*modulesList)   out.command = Command::ModulesList;
	else if (*modulesDump)   out.command = Command::ModulesDump;
	else if (*modulesExplain) out.command = Command::ModulesExplain;
	else if (*romInfo)       out.command = Command::RomInfo;
	else if (*romFiles)      out.command = Command::RomFiles;
	else if (*romExtract)    out.command = Command::RomExtract;
	else if (*romPack)       out.command = Command::RomPack;
	else if (*filesPlan)     out.command = Command::FilesPlan;
	else if (*version)       out.command = Command::Version;
	else if (*build)         out.command = Command::Build;
	else
	{
		std::cout << "ncpatcher " << versionString() << "\n"
		          << "Compiles C, C++ and assembly and splices the result into a "
		             "Nintendo DS ROM.\n\n"
		          << "  ncpatcher build     compile and patch the ROM\n"
		          << "  ncpatcher --help    every command and option\n";
		return 0;
	}

	if (out.command == Command::Version)
	{
		std::cout << "ncpatcher " << versionString() << std::endl;
		return 0;
	}

	out.toolchainSource = app.count("--toolchain") > 0 ?
		config::Source::CommandLine : config::Source::Default;
	out.jobsSource = app.count("--jobs") > 0 ?
		config::Source::CommandLine : config::Source::Default;
	out.logPathSet = app.count("--log") > 0;

	// The environment fills in only what the command line did not say, and is
	// recorded as such: `config dump --explain` has to be able to answer "why
	// is it using that compiler" with more than a shrug.
	if (out.toolchainSource == config::Source::Default)
	{
		if (const auto value = environment("NCPATCHER_TOOLCHAIN"))
		{
			out.toolchain = *value;
			out.toolchainSource = config::Source::Environment;
		}
	}

	if (out.jobsSource == config::Source::Default)
	{
		if (const auto value = environment("NCPATCHER_JOBS"))
		{
			try {
				out.jobs = std::stoi(*value);
				out.jobsSource = config::Source::Environment;
			} catch (const std::exception&) {
				std::cerr << "NCPATCHER_JOBS is not a number: " << *value << std::endl;
				return exitValue(ExitCode::Usage);
			}
		}
	}

	if (!out.logPathSet && !out.noLog)
	{
		if (const auto value = environment("NCPATCHER_LOG"))
		{
			out.logPath = *value;
			out.logPathSet = true;
		}
	}

	// The de-facto convention, honored because a build tool's output ends up
	// in CI logs far more often than on a terminal.
	if (app.count("--color") == 0 && environment("NO_COLOR"))
		out.color = Log::ColorMode::Never;

	return std::nullopt;
}

} // namespace ncp
