#include "application.hpp"

#include <iostream>
#include <sstream>
#include <cstring>
#include <unordered_set>

#include "../system/log.hpp"
#include "../system/process.hpp"
#include "../system/except.hpp"
#include "../system/cache.hpp"
#include "../utils/types.hpp"
#include "../config/buildconfig.hpp"
#include "../config/buildtarget.hpp"
#include "../config/rebuildconfig.hpp"
#include "../ndsbin/headerbin.hpp"
#include "../build/objmaker.hpp"
#include "../patch/patchmaker.hpp"
#include "../core/compilation_unit_manager.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include <windows.h>
#elif __linux__
#include <unistd.h>
#elif __APPLE__
#include <mach-o/dyld.h>
#include <limits.h>
#else
#error Unsupported operating system
#endif

namespace fs = std::filesystem;

namespace ncp {

// Static member definitions
std::filesystem::path Application::s_appPath;
std::filesystem::path Application::s_workPath;
std::filesystem::path Application::s_romPath;
std::vector<std::string> Application::s_defines;
std::unordered_set<VerboseTag> Application::s_verboseTags;
const char* Application::s_errorContext = nullptr;

Application::Application() = default;
Application::~Application() = default;

int Application::initialize(int argc, char* argv[])
{
    Log::init();

    try {
        s_appPath = fetchAppPath();
    } catch (std::exception& ex) {
        Log::error(ex.what());
        return 1;
    }

    try {
        initializePaths();
    } catch (std::exception& ex) {
        Log::error("Could not initialize application paths.");
        return 1;
    }

    try {
        initializeLogging();
    } catch (std::exception& ex) {
        Log::error("Could not open the log file for writing.");
        return 1;
    }

    // Initialize caches
    ncp::cache::CacheManager::getInstance().clearCaches();

    if (!parseCommandLineArgs(argc, argv)) {
        return 1;
    }

    return 0;
}

int Application::run()
{
    try {
        runMainLogic();
    } catch (std::exception& e) {
        Log::out << OERROR;
        if (s_errorContext) {
            Log::out << s_errorContext << "\n" << OREASON;
        }
        Log::out << e.what() << std::endl;
        return 1;
    }

    return 0;
}

void Application::runMainLogic()
{
    Log::out << ANSI_bWHITE " ----- Nitro Code Patcher -----" ANSI_RESET << std::endl;

    loadConfigurations();
    validateToolchain();

    s_romPath = fs::absolute(BuildConfig::getFilesystemDir());

    HeaderBin header;
    header.load(s_romPath / "header.bin");

    bool forceRebuild = checkForceRebuild();

    runCommandList(BuildConfig::getPreBuildCmds(), 
                   "Running pre-build commands...", 
                   "Not all pre-build commands succeeded.");

    if (BuildConfig::getBuildArm7()) {
        processTarget(header, false); // ARM7
    }

    if (BuildConfig::getBuildArm9()) {
        processTarget(header, true);  // ARM9
    }

    saveRebuildConfig();

    runCommandList(BuildConfig::getPostBuildCmds(), 
                   "Running post-build commands...", 
                   "Not all post-build commands succeeded.");

    Log::info("All tasks finished.");
}

void Application::processTarget(HeaderBin& header, bool isArm9)
{
    fs::current_path(getWorkPath());

    Log::info(isArm9 ? 
        "Loading ARM9 target configuration..." :
        "Loading ARM7 target configuration...");

    const fs::path& targetPath = fs::absolute(isArm9 ? 
        BuildConfig::getArm9Target() : 
        BuildConfig::getArm7Target());

    setErrorContext(isArm9 ?
        "Could not load the ARM9 target configuration." :
        "Could not load the ARM7 target configuration.");
    
    BuildTarget buildTarget;
    buildTarget.load(targetPath, isArm9);
    setErrorContext(nullptr);

    std::time_t lastTargetWriteTimeNew = buildTarget.getLastWriteTime();
    std::time_t lastTargetWriteTimeOld = isArm9 ?
        RebuildConfig::getArm9TargetWriteTime() :
        RebuildConfig::getArm7TargetWriteTime();
    
    bool forceRebuild = checkForceRebuild();
    buildTarget.setForceRebuild(forceRebuild || (lastTargetWriteTimeNew > lastTargetWriteTimeOld));

    setErrorContext(isArm9 ?
        "Could not compile the ARM9 target." :
        "Could not compile the ARM7 target.");

    fs::path targetDir = targetPath.parent_path();
    fs::path buildPath = fs::absolute(isArm9 ? 
        BuildConfig::getArm9BuildDir() : 
        BuildConfig::getArm7BuildDir());

    core::CompilationUnitManager compilationUnitsMgr;

    ObjMaker objMaker;
    objMaker.makeTarget(buildTarget, targetDir, buildPath, compilationUnitsMgr);

    PatchMaker patchMaker;
    patchMaker.makeTarget(buildTarget, targetDir, buildPath, header, compilationUnitsMgr);

    // Update rebuild config
    if (isArm9) {
        RebuildConfig::setArm9TargetWriteTime(lastTargetWriteTimeNew);
    } else {
        RebuildConfig::setArm7TargetWriteTime(lastTargetWriteTimeNew);
    }

    setErrorContext(nullptr);
}

void Application::runCommandList(const std::vector<std::string>& commands, 
                                const char* message, 
                                const char* errorContext)
{
    if (commands.empty()) {
        return;
    }

    Log::info(message);
    setErrorContext(errorContext);

    int commandIndex = 1;
    for (const std::string& command : commands) {
        std::ostringstream oss;
        oss << ANSI_bWHITE "[#" << commandIndex << "] " ANSI_bYELLOW << command << ANSI_RESET;
        Log::info(oss.str());

        fs::current_path(getWorkPath());

        int retcode = Process::start(command.c_str(), &std::cout);
        if (retcode != 0) {
            throw ncp::exception("Process returned: " + std::to_string(retcode));
        }
        
        commandIndex++;
    }

    setErrorContext(nullptr);
}

void Application::loadConfigurations()
{
    BuildConfig::load();
    RebuildConfig::load();
}

bool Application::checkForceRebuild()
{
    return BuildConfig::getLastWriteTime() > RebuildConfig::getBuildConfigWriteTime() ||
           getDefines() != RebuildConfig::getDefines();
}

void Application::saveRebuildConfig()
{
    RebuildConfig::setBuildConfigWriteTime(BuildConfig::getLastWriteTime());
    RebuildConfig::setDefines(getDefines());
    RebuildConfig::save();
}

void Application::validateToolchain()
{
    const std::string& toolchain = BuildConfig::getToolchain();
    std::string gccPath = toolchain + "gcc";
    
    if (!Process::exists(gccPath.c_str())) {
        std::ostringstream oss;
        oss << "The building toolchain " << OSTR(toolchain) << " was not found." << OREASONNL;
        oss << "Make sure that it is correctly specified in the " << OSTR("ncpatcher.json") 
            << " file and that it is present on your system.";
        throw ncp::exception(oss.str());
    }
}

void Application::initializePaths()
{
    s_workPath = fs::current_path();
}

void Application::initializeLogging()
{
    Log::openLogFile(s_appPath / "log.txt");
}

std::filesystem::path Application::fetchAppPath()
{
    // Copied from arclight.filesystem

#ifdef _WIN32

    u32 length = 0x200;
    std::vector<wchar_t> filename;

    try {
        filename.resize(length);
        while (GetModuleFileNameW(nullptr, filename.data(), length) == length) {
            if (length < 0x8000) {
                length *= 2;
                filename.resize(length);
            } else {
                /*
                    Ideally, this cannot happen because the windows path limit is specified to be 0x7FFF (excl. null terminator byte)
                    If this changes in future windows versions, long path names could fail since it would require to allocate fairly large buffers
                    This is why we stop here with an error.
                */
                throw std::runtime_error("Could not query application directory path: Path too long");
            }
        }

        std::wstring str(filename.data());
        return std::filesystem::path(str).parent_path();
    } catch (std::exception& e) {
        throw std::runtime_error(std::string("Could not query application directory path: ") + e.what());
    }

#elif __linux__

    constexpr const char* symlinkName = "/proc/self/exe";
    SizeT length = 0x200;

    std::vector<char> filename(length);

    try {
        while(true) {
            ssize_t readLength = readlink(symlinkName, filename.data(), filename.size());

            if (readLength == length) {
                //If length exceeds 0x10000 bytes, cancel
                if(length >= 0x10000) {
                    throw std::runtime_error("Could not query application directory path: Path name exceeds 0x10000 bytes");
                }

                //Double buffer and retry
                length *= 2;
                filename.resize(length);
            } else if (readLength == -1) {
                //Error occured while reading the symlink
                throw std::runtime_error("Could not query application directory path: Cannot read symbolic link");
            } else {
                //Read was successful, return filename
                std::string str(filename.data(), readLength);
                return std::filesystem::path(str).parent_path();
            }
        }
    } catch (std::exception& e) {
        throw std::runtime_error(std::string("Could not query application directory path: ") + e.what());
    }

#elif __APPLE__

    char buf[PATH_MAX];
    uint32_t bufsize = PATH_MAX;
    if (_NSGetExecutablePath(buf, &bufsize) != 0) {
        throw std::runtime_error("Could not query application directory path.");
    }
    return std::filesystem::path(buf).parent_path();

#endif
}

bool Application::parseCommandLineArgs(int argc, char* argv[])
{
    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "--help") == 0) || (strcmp(argv[i], "-h") == 0)) {
            printHelp();
            return false; // Exit successfully after showing help
        } else if ((strcmp(argv[i], "--verbose") == 0) || (strcmp(argv[i], "-v") == 0)) {
            // Enables all verbose output
            s_verboseTags.insert(VerboseTag::All);
        } else if (strcmp(argv[i], "--verbose-tag") == 0) {
            if (i + 1 < argc) {
                std::string tagName = argv[i + 1];
                VerboseTag tag = parseVerboseTag(tagName);
                if (tag != static_cast<VerboseTag>(-1)) {
                    s_verboseTags.insert(tag);
                } else {
                    std::ostringstream oss;
                    oss << "Unknown verbose tag: " << tagName;
                    Log::error(oss.str());
                    return false;
                }
                i++; // Skip the next argument since we consumed it
            } else {
                Log::error("--verbose-tag option requires a tag name");
                return false;
            }
        } else if (strcmp(argv[i], "--define") == 0) {
            if (i + 1 < argc) {
                s_defines.push_back(argv[i + 1]);
                i++; // Skip the next argument since we consumed it
            } else {
                Log::error("--define option requires a value");
                return false;
            }
        } else {
            std::ostringstream oss;
            oss << "Unknown argument: " << argv[i];
            Log::error(oss.str());
            Log::out << std::endl;
            Log::out << "Use --help or -h to see available options." << std::endl;
            return false;
        }
    }
    return true;
}

VerboseTag Application::parseVerboseTag(const std::string& tagName)
{
    if (tagName == "build") return VerboseTag::Build;
    if (tagName == "section") return VerboseTag::Section;
    if (tagName == "elf") return VerboseTag::Elf;
    if (tagName == "patch") return VerboseTag::Patch;
    if (tagName == "library") return VerboseTag::Library;
    if (tagName == "linking") return VerboseTag::Linking;
    if (tagName == "symbols") return VerboseTag::Symbols;
    if (tagName == "nolib") return VerboseTag::NoLib;
    if (tagName == "all") return VerboseTag::All;
    
    return static_cast<VerboseTag>(-1); // Invalid tag
}

void Application::printHelp()
{
    Log::out << ANSI_bWHITE " ----- Nitro Code Patcher -----" ANSI_RESET << std::endl;
    Log::out << std::endl;
    Log::out << "Usage: ncpatcher [options]" << std::endl;
    Log::out << std::endl;
    Log::out << "Options:" << std::endl;
    Log::out << "  -h, --help       Show this help message and exit" << std::endl;
    Log::out << "  -v, --verbose    Enable all verbose logging output (legacy)" << std::endl;
    Log::out << "  --verbose-tag TAG  Enable verbose output for specific category:" << std::endl;
    Log::out << "                     build     - Build process and compilation" << std::endl;
    Log::out << "                     section   - Section usage analysis" << std::endl;
    Log::out << "                     elf       - ELF file processing" << std::endl;
    Log::out << "                     patch     - Patch information and analysis" << std::endl;
    Log::out << "                     library   - Library dependency analysis" << std::endl;
    Log::out << "                     linking   - Linker script generation" << std::endl;
    Log::out << "                     symbols   - Symbol resolution" << std::endl;
    Log::out << "                     all       - All verbose output" << std::endl;
    Log::out << "                     (Multiple --verbose-tag options can be used)" << std::endl;
    Log::out << "  --define VALUE   Define a preprocessor macro for compilation" << std::endl;
    Log::out << std::endl;
    Log::out << "Description:" << std::endl;
    Log::out << "  NCPatcher is a tool for patching Nintendo DS ROMs by compiling" << std::endl;
    Log::out << "  and injecting custom ARM7/ARM9 code into the ROM filesystem." << std::endl;
    Log::out << std::endl;
    Log::out << "  The tool reads configuration from 'ncpatcher.json' in the current" << std::endl;
    Log::out << "  directory and processes ARM7/ARM9 targets as specified." << std::endl;
}

// Static getters
const std::filesystem::path& Application::getAppPath() 
{ 
    return s_appPath; 
}

const std::filesystem::path& Application::getWorkPath() 
{ 
    return s_workPath; 
}

const std::filesystem::path& Application::getRomPath() 
{ 
    return s_romPath; 
}

bool Application::isVerbose(VerboseTag tag)
{
    return s_verboseTags.count(VerboseTag::All) > 0 || s_verboseTags.count(tag) > 0;
}

const std::vector<std::string>& Application::getDefines() 
{ 
    return s_defines; 
}

void Application::setErrorContext(const char* errorContext) 
{ 
    s_errorContext = errorContext; 
}

} // namespace ncp
