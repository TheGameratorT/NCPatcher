#pragma once

#include <filesystem>
#include <string>
#include <vector>

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

    // Initialize the application with command line arguments
    int initialize(int argc, char* argv[]);

    // Run the main application logic
    int run();

private:
    // Everything the build reads, owned here and handed down by reference.
    // These used to be file-scope statics in BuildConfig and Application; see
    // context.hpp for why they are not any more.
    Options m_options;
    config::ProjectConfig m_config;
    config::RebuildStore m_rebuild;
    Context m_ctx;

    // What the invocation asked for beyond a plain build.
    enum class Command { Build, Migrate };
    Command m_command = Command::Build;
    bool m_migrateWrite = false;

    // Core application methods
    void runMainLogic();
    int runMigrate();
    static void reportFailure(const std::exception& e);
    void processTarget(HeaderBin& header, bool isArm9);
    void runCommandList(const std::vector<std::string>& commands,
                       const char* message,
                       Diag code,
                       const char* errorContext);

    // Initialization helpers
    static std::filesystem::path fetchAppPath();
    void printHelp();
    bool parseCommandLineArgs(int argc, char* argv[]);
    VerboseTag parseVerboseTag(const std::string& tagName);
    void initializePaths();
    void initializeLogging();
    void validateToolchain();

    // Configuration management
    [[nodiscard]] std::filesystem::path projectFile() const;
    void loadConfigurations();
    void saveRebuildConfig();
};

} // namespace ncp
