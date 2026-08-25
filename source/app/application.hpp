#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "cli.hpp"
#include "config_dump.hpp"
#include "context.hpp"
#include "../config/project_config.hpp"
#include "../config/config_loader.hpp"
#include "../config/env_file.hpp"
#include "../config/rebuild_store.hpp"
#include "../modules/module_graph.hpp"
#include "../system/diagnostics.hpp"

#include "../rom/dir_accessor.hpp"

namespace ncp::rom { class RomAccessor; }

namespace ncp {

class Application
{
public:
	Application();
	~Application();

	// Sets up logging, parses the command line and works out where the project
	// is. Returns an exit code when the process should stop here -- because
	// --help was asked for, or because the command line did not parse -- and
	// nothing when run() should be called.
	[[nodiscard]] std::optional<int> initialize(int argc, char* argv[]);

	// Runs the requested subcommand and returns the process exit code.
	[[nodiscard]] int run();

private:
	// Everything the build reads, owned here and handed down by reference.
	// These used to be file-scope statics in BuildConfig and Application; see
	// context.hpp for why they are not any more.
	CommandLine m_cli;
	Options m_options;
	config::ProjectConfig m_config;
	config::RebuildStore m_rebuild;

	// Loaded before the configuration and kept for the whole run: the config
	// expander borrows it, and hook children inherit its entries.
	config::EnvFile m_envFile;

	// The variant this build is for, empty when the project has none. Kept
	// because hooks name it: see resolveDeferred().
	std::string m_variant;

	Context m_ctx;

	// What the enabled modules add up to. Empty for a project without a
	// `modules:` section, which is every project that exists today.
	modules::ModuleGraph m_modules;

	// Subcommands
	void runBuild();
	void runConfiguredBuild();
	int runInit();
	int runClean();
	int runRestore();
	int runConfigDump();
	int runConfigValidate();
	int runConfigPath();
	int runMigrate();
	int runModulesCommand();
	int runRomCommand();

	static int reportFailure(const std::exception& e);
	void processTarget(ncp::rom::RomAccessor& rom, bool isArm9);

	// Applies inheritance and expands globs for one target, and fills in the
	// per-target path anchors. Shared by the build and by `config dump`, so
	// that what the dump prints is what the build would use rather than a
	// second implementation of the same rules.
	[[nodiscard]] ResolvedTarget resolveTarget(bool isArm9, Context& targetCtx) const;

	void runHooks(config::HookWhen when,
				  const char* message,
				  Diag code,
				  const char* errorContext);
	// What one insertion pass did. The manifest needs both halves: the
	// resolved list says where a file's bytes came from, and the ids say which
	// of those files the ROM did not already have -- by the time the manifest
	// is written, a created file and a replaced one look alike.
	struct InsertedFiles
	{
		std::vector<config::FileConfig> files;
		std::vector<u32> createdIds;
	};

	InsertedFiles insertFiles(ncp::rom::RomAccessor& rom);
	void writeFileDump(const ncp::rom::RomAccessor& rom, const InsertedFiles& inserted) const;

	// Overwrites the ROM's icon/title banner, if the project supplies one. Its
	// own step because a banner is not a NitroFS file: it is a region the
	// header points at, so no path names it and `files:` cannot carry it.
	void insertBanner(ncp::rom::RomAccessor& rom) const;

	// The project's `files:` folded onto whatever the file trees sweep up.
	[[nodiscard]] std::vector<config::FileConfig> resolveNitroFiles() const;

	// Initialization helpers
	void initializePaths();
	void initializeLogging();
	[[nodiscard]] std::filesystem::path logDirectory() const;
	void openDefaultLogFile();
	void validateToolchain();

	// Configuration management
	[[nodiscard]] std::filesystem::path projectFile() const;
	[[nodiscard]] config::V1Options targetLoadOptions() const;
	[[nodiscard]] std::string resolveDeferred(std::string text) const;
	void loadConfigurations();

	// Reads the module.yaml files and folds them into m_modules.
	//
	// Called before target resolution, because a module decides which sources a
	// region has, and before the pre-build commands, because the dump it writes
	// is what a code generator run as a hook consumes.
	void loadModules(bool quiet = false);
	void writeModuleDump();

	void applyCommandLineOverrides();
	void applyVariant(const std::string& name, bool deriveOutput);
	void resolveRomDir();

	// How much room to leave after the ARM9 binary when a .nds has to be laid
	// out again. 64 KiB is comfortably more than any project has ever added to
	// ARM9, and costs that much dead space once rather than a full relayout on
	// every build.
	static constexpr u32 DEFAULT_ARM9_SLACK = 0x10000;

	[[nodiscard]] static bool looksLikeRomFile(const std::filesystem::path& path);

	// The ROM the `rom` subcommands act on, and the layout they read it with.
	// --rom on its own is enough: those commands are useful outside a project,
	// and requiring a configuration file to look at a ROM would be silly.
	[[nodiscard]] std::filesystem::path romTarget();
	[[nodiscard]] ncp::rom::DirLayout romLayout() const;

	// Builds the accessor the whole build patches through: the extracted
	// directory or, once rom.file names one, the .nds itself.
	[[nodiscard]] std::unique_ptr<ncp::rom::RomAccessor> openRom();

	// The .nds this build patches, absolute. Empty when the project patches an
	// extracted directory instead.
	std::filesystem::path m_romFile;
	void saveRebuildConfig();
};

} // namespace ncp
