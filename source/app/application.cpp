#include "application.hpp"

#include <iostream>
#include <sstream>
#include <cstring>
#include <unordered_set>

#include "../system/log.hpp"
#include "../system/process.hpp"
#include "../system/except.hpp"
#include "../system/cache.hpp"
#include "../system/diagnostics.hpp"
#include "../utils/types.hpp"
#include "../config/buildtarget.hpp"
#include "../config/config_loader.hpp"
#include "../config/migrate.hpp"
#include "../config/target_resolver.hpp"
#include "../utils/hash.hpp"
#include "../ndsbin/headerbin.hpp"
#include "../build/objmaker.hpp"
#include "../patch/patch_maker.hpp"
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

Application::Application() = default;
Application::~Application() = default;

int Application::initialize(int argc, char* argv[])
{
    Log::init();

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

    // The context is assembled once and then only ever copied: the per-target
    // copies adjust their path anchors and share everything else.
    m_ctx.config = &m_config;
    m_ctx.options = &m_options;
    m_ctx.rebuild = &m_rebuild;

    return 0;
}

int Application::run()
{
    try {
        if (m_command == Command::Migrate)
            return runMigrate();
        runMainLogic();
    } catch (std::exception& e) {
        reportFailure(e);
        return 1;
    }

    return 0;
}

int Application::runMigrate()
{
    ScopedContext ctx(Diag::ConfigMigrate, "Could not migrate the configuration.");

    const fs::path file = projectFile();
    return config::migrate(file, m_ctx.paths.workDir, m_migrateWrite) ? 0 : 1;
}

// Renders a failure as the phase it happened in, the reason, and then any
// enclosing phases. The innermost context is the headline because it is the
// most specific thing that was being attempted.
void Application::reportFailure(const std::exception& e)
{
    const std::vector<DiagContext>& contexts = diagnostics::failureContext();

    Log::out << OERROR;
    if (!contexts.empty())
        Log::out << diagCode(contexts.front().code) << ": " << contexts.front().description << "\n" << OREASON;
    Log::out << e.what() << std::endl;

    for (std::size_t i = 1; i < contexts.size(); i++)
        Log::out << "        while " << diagCode(contexts[i].code) << ": " << contexts[i].description << std::endl;

    diagnostics::clearFailureContext();
}

void Application::runMainLogic()
{
    Log::out << ANSI_bWHITE " ----- Nitro Code Patcher -----" ANSI_RESET << std::endl;

    loadConfigurations();
    validateToolchain();

    if (m_config.romFile.configured()) {
        std::ostringstream oss;
        oss << "Reading a ROM file directly is not supported by this version of NCPatcher."
            << OREASONNL << "Extract the ROM and point " << OSTRa("rom.dir") << " at the result.";
        throw ncp::exception(oss.str());
    }

    m_ctx.paths.romDir = m_ctx.paths.work(m_config.filesystemDir.value);

    HeaderBin header;
    header.load(m_ctx.paths.romDir / "header.bin");

    runCommandList(m_config.preBuild,
                   "Running pre-build commands...",
                   Diag::PreBuildCommand,
                   "Not all pre-build commands succeeded.");

    // Only now: a v1 project may have just generated its target files.
    {
        ScopedContext ctx(Diag::TargetConfigLoad, "Could not load the target configuration.");
        config::loadTargets(m_config);
    }

    if (m_config.arm7.enabled) {
        processTarget(header, false); // ARM7
    }

    if (m_config.arm9.enabled) {
        processTarget(header, true);  // ARM9
    }

    saveRebuildConfig();

    runCommandList(m_config.postBuild,
                   "Running post-build commands...",
                   Diag::PostBuildCommand,
                   "Not all post-build commands succeeded.");

    Log::info("All tasks finished.");
}

void Application::processTarget(HeaderBin& header, bool isArm9)
{
    Log::info(isArm9 ?
        "Resolving ARM9 target configuration..." :
        "Resolving ARM7 target configuration...");

    const config::TargetConfig& targetConfig = m_config.target(isArm9);

    // Per-target anchors. A copy, so the two targets cannot see each other's.
    Context targetCtx = m_ctx;
    targetCtx.paths.targetWorkDir = targetConfig.workDir.configured() ?
        m_ctx.paths.work(targetConfig.workDir.value) :
        targetConfig.file.parent_path();
    targetCtx.paths.buildDir = m_ctx.paths.work(targetConfig.buildDir.value);

    BuildTarget buildTarget;
    {
        ScopedContext ctx(Diag::TargetConfigLoad, isArm9 ?
            "Could not resolve the ARM9 target configuration." :
            "Could not resolve the ARM7 target configuration.");
        buildTarget = config::TargetResolver::resolve(m_config, targetConfig, targetCtx.paths);
    }

    // Staleness is a question about the resolved configuration, not about when
    // a file was last touched: what matters is whether the objects on disk were
    // compiled under the rules this build is using.
    const std::string configHash = Hash::of(config::TargetResolver::describe(m_config, buildTarget));
    BuildTargetBuilder::setConfigHash(buildTarget, configHash);

    const bool projectChanged = m_rebuild.projectChanged(
        Hash::of(config::TargetResolver::describeProject(m_config, m_options.defines)));
    buildTarget.setForceRebuild(projectChanged || m_rebuild.targetChanged(isArm9, configHash));

    ScopedContext ctx(Diag::TargetCompile, isArm9 ?
        "Could not compile the ARM9 target." :
        "Could not compile the ARM7 target.");

    core::CompilationUnitManager compilationUnitsMgr;

    ObjMaker objMaker;
    objMaker.makeTarget(buildTarget, targetCtx, compilationUnitsMgr);

    ncp::patch::PatchMaker patchMaker;
    patchMaker.makeTarget(buildTarget, targetCtx, header, compilationUnitsMgr);

    m_rebuild.setTargetHash(isArm9, configHash);
}

void Application::runCommandList(const std::vector<std::string>& commands,
                                const char* message,
                                Diag code,
                                const char* errorContext)
{
    if (commands.empty()) {
        return;
    }

    Log::info(message);
    ScopedContext ctx(code, errorContext);

    int commandIndex = 1;
    for (const std::string& command : commands) {
        std::ostringstream oss;
        oss << ANSI_bWHITE "[#" << commandIndex << "] " ANSI_bYELLOW << command << ANSI_RESET;
        Log::info(oss.str());

        int retcode = Process::start(command.c_str(), m_ctx.paths.workDir, &std::cout);
        if (retcode != 0) {
            throw ncp::exception("Process returned: " + std::to_string(retcode));
        }
        
        commandIndex++;
    }
}

std::filesystem::path Application::projectFile() const
{
    const fs::path file = config::findProjectFile(m_ctx.paths.workDir);
    if (file.empty()) {
        std::ostringstream oss;
        oss << "No NCPatcher configuration was found in " << OSTR(m_ctx.paths.workDir.string()) << "."
            << OREASONNL << "Expected " << OSTRa("ncpatcher.yaml") << " or " << OSTRa("ncpatcher.json") << ".";
        throw ncp::exception(oss.str());
    }
    return file;
}

void Application::loadConfigurations()
{
    ScopedContext ctx(Diag::ConfigLoad, "Could not load the build configuration.");

    Log::info("Loading build configuration...");
    m_config = config::load(projectFile(), m_ctx.paths.workDir);

    m_rebuild.load(m_ctx.backupDir() / "rebuild.json");
}

void Application::saveRebuildConfig()
{
    m_rebuild.setProjectHash(
        Hash::of(config::TargetResolver::describeProject(m_config, m_options.defines)));
    m_rebuild.save(m_ctx.backupDir() / "rebuild.json");
}

void Application::validateToolchain()
{
    const std::string& toolchain = m_ctx.toolchain();
    std::string gccPath = toolchain + "gcc";

    if (!Process::exists(gccPath.c_str())) {
        std::ostringstream oss;
        oss << "The building toolchain " << OSTR(toolchain) << " was not found." << OREASONNL;
        oss << "Make sure that it is correctly specified in "
            << OSTR(m_config.file.filename().string())
            << " and that it is present on your system.";
        throw ncp::exception(oss.str());
    }
}

void Application::initializePaths()
{
    // The cwd is read exactly once, here, and is never written. Everything
    // downstream resolves against m_ctx.paths instead.
    m_ctx.paths.appDir = fetchAppPath();
    m_ctx.paths.workDir = fs::current_path();
}

void Application::initializeLogging()
{
    Log::openLogFile(m_ctx.paths.appDir / "log.txt");
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
    int first = 1;

    // One subcommand, recognised only in first position, so that a project with
    // a source directory called "migrate" cannot be mistaken for one.
    if (argc > 1 && strcmp(argv[1], "migrate") == 0) {
        m_command = Command::Migrate;
        first = 2;
    }

    for (int i = first; i < argc; i++) {
        if ((strcmp(argv[i], "--help") == 0) || (strcmp(argv[i], "-h") == 0)) {
            printHelp();
            return false; // Exit successfully after showing help
        } else if ((strcmp(argv[i], "--verbose") == 0) || (strcmp(argv[i], "-v") == 0)) {
            // Enables all verbose output
            m_options.verboseTags.insert(VerboseTag::All);
        } else if (strcmp(argv[i], "--verbose-tag") == 0) {
            if (i + 1 < argc) {
                std::string tagName = argv[i + 1];
                VerboseTag tag = parseVerboseTag(tagName);
                if (tag != static_cast<VerboseTag>(-1)) {
                    m_options.verboseTags.insert(tag);
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
                m_options.defines.push_back(argv[i + 1]);
                i++; // Skip the next argument since we consumed it
            } else {
                Log::error("--define option requires a value");
                return false;
            }
        } else if (m_command == Command::Migrate && strcmp(argv[i], "--write") == 0) {
            m_migrateWrite = true;
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
    Log::out << "       ncpatcher migrate [--write]" << std::endl;
    Log::out << std::endl;
    Log::out << "Commands:" << std::endl;
    Log::out << "  migrate          Convert a version 1 ncpatcher.json to ncpatcher.yaml" << std::endl;
    Log::out << "                   Prints the result; --write saves it" << std::endl;
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
    Log::out << "  The tool reads 'ncpatcher.yaml' from the current directory, falling" << std::endl;
    Log::out << "  back to the version 1 'ncpatcher.json', and processes the ARM7 and" << std::endl;
    Log::out << "  ARM9 targets it specifies." << std::endl;
}

} // namespace ncp
