#pragma once

#include <filesystem>
#include <vector>
#include <string>
#include <unordered_set>

#include "../system/path_context.hpp"
#include "../system/diagnostics.hpp"

class HeaderBin;

namespace ncp {

// Verbose output categories
enum class VerboseTag {
    Build,      // Build process and compilation output
    Section,    // Section usage analysis and details  
    Elf,        // ELF file analysis and processing
    Patch,      // Patch information and analysis
    Library,    // Library dependency analysis
    Linking,    // Linker script generation and linking process
    Symbols,    // Symbol resolution and analysis
	NoLib,      // Do not print lib patches
    All         // All verbose output (equivalent to old --verbose)
};

class Application
{
public:
    Application();
    ~Application();

    // Initialize the application with command line arguments
    int initialize(int argc, char* argv[]);
    
    // Run the main application logic
    int run();

    // Static getters for application configuration
    static bool isVerbose(VerboseTag tag);
    static const std::vector<std::string>& getDefines();

private:
    // The directories every relative path in the build resolves against.
    // Owned here and passed down explicitly; nothing changes the process cwd.
    PathContext m_paths;

    // Application configuration
    static std::vector<std::string> s_defines;
    static std::unordered_set<VerboseTag> s_verboseTags;

    // Core application methods
    void runMainLogic();
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
    void loadConfigurations();
    bool checkForceRebuild();
    void saveRebuildConfig();
};

} // namespace ncp
