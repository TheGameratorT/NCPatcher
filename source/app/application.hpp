#pragma once

#include <filesystem>
#include <vector>
#include <string>
#include <unordered_set>

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

    // Static getters for application paths and configuration
    static const std::filesystem::path& getAppPath();
    static const std::filesystem::path& getWorkPath();
    static const std::filesystem::path& getRomPath();
    static bool isVerbose(VerboseTag tag);
    static const std::vector<std::string>& getDefines();
    
    // Error context management
    static void setErrorContext(const char* errorContext);

private:
    // Application paths
    static std::filesystem::path s_appPath;
    static std::filesystem::path s_workPath;
    static std::filesystem::path s_romPath;
    
    // Application configuration
    static std::vector<std::string> s_defines;
    static std::unordered_set<VerboseTag> s_verboseTags;
    static const char* s_errorContext;

    // Core application methods
    void runMainLogic();
    void processTarget(HeaderBin& header, bool isArm9);
    void runCommandList(const std::vector<std::string>& commands, 
                       const char* message, 
                       const char* errorContext);
    
    // Initialization helpers
    std::filesystem::path fetchAppPath();
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
