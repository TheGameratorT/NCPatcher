#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "cli.hpp"
#include "config_dump.hpp"
#include "context.hpp"
#include "../config/project_config.hpp"
#include "../config/rebuild_store.hpp"
#include "../system/diagnostics.hpp"

class HeaderBin;

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
    Context m_ctx;

    // Subcommands
    void runBuild();
    int runClean();
    int runRestore();
    int runConfigDump();
    int runConfigValidate();
    int runConfigPath();
    int runMigrate();

    static int reportFailure(const std::exception& e);
    void processTarget(HeaderBin& header, bool isArm9);

    // Applies inheritance and expands globs for one target, and fills in the
    // per-target path anchors. Shared by the build and by `config dump`, so
    // that what the dump prints is what the build would use rather than a
    // second implementation of the same rules.
    [[nodiscard]] ResolvedTarget resolveTarget(bool isArm9, Context& targetCtx) const;

    void runCommandList(const std::vector<std::string>& commands,
                       const char* message,
                       Diag code,
                       const char* errorContext);

    // Initialization helpers
    void initializePaths();
    void initializeLogging();
    [[nodiscard]] std::filesystem::path logDirectory() const;
    void openDefaultLogFile();
    void validateToolchain();

    // Configuration management
    [[nodiscard]] std::filesystem::path projectFile() const;
    void loadConfigurations();
    void applyCommandLineOverrides();
    void resolveRomDir();
    void saveRebuildConfig();
};

} // namespace ncp
