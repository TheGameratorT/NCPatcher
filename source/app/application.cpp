#include "application.hpp"

#include <iostream>
#include <sstream>

#include "../system/ansi.hpp"
#include "../system/log.hpp"
#include "../system/process.hpp"
#include "../system/except.hpp"
#include "../system/cache.hpp"
#include "../system/diagnostics.hpp"
#include "../system/exit_code.hpp"
#include "../system/message.hpp"
#include "../utils/types.hpp"
#include "../utils/json.hpp"
#include "../config/buildtarget.hpp"
#include "../config/config_loader.hpp"
#include "../config/migrate.hpp"
#include "../config/target_resolver.hpp"
#include "config_dump.hpp"
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

std::optional<int> Application::initialize(int argc, char* argv[])
{
    Log::init();

    if (const std::optional<int> stop = parseCommandLine(argc, argv, m_cli))
        return stop;

    // Console styling and the destination of the human log are decided before
    // anything else can print. Two things move the log to stderr: json output,
    // whose event stream has to be the only thing on stdout, and the commands
    // whose product *is* stdout -- a `config dump` with a deprecation warning
    // mixed into it is not something a caller can pipe anywhere.
    const bool logToStderr =
        m_cli.messageFormat == msg::Format::Json ||
        m_cli.command == Command::ConfigDump ||
        m_cli.command == Command::ConfigPath;
    Log::configureConsole(m_cli.color, logToStderr);
    msg::configure(m_cli.messageFormat, m_cli.resultPath);

    try {
        initializePaths();
    } catch (std::exception& ex) {
        Log::error(std::string("Could not initialize application paths.") + OREASONNL + ex.what());
        return exitValue(ExitCode::Internal);
    }

    try {
        initializeLogging();
    } catch (std::exception& ex) {
        Log::error("Could not open the log file for writing.");
        return exitValue(ExitCode::Internal);
    }

    // Initialize caches
    ncp::cache::CacheManager::getInstance().clearCaches();

    m_options.defines = m_cli.defines;
    m_options.verboseTags = m_cli.verboseTags;

    // The context is assembled once and then only ever copied: the per-target
    // copies adjust their path anchors and share everything else.
    m_ctx.config = &m_config;
    m_ctx.options = &m_options;
    m_ctx.rebuild = &m_rebuild;

    return std::nullopt;
}

int Application::run()
{
    int code = exitValue(ExitCode::Ok);

    try {
        switch (m_cli.command)
        {
        case Command::Clean:          code = runClean(); break;
        case Command::Restore:        code = runRestore(); break;
        case Command::ConfigDump:     code = runConfigDump(); break;
        case Command::ConfigValidate: code = runConfigValidate(); break;
        case Command::ConfigPath:     code = runConfigPath(); break;
        case Command::Migrate:        code = runMigrate(); break;
        default:                      runBuild(); break;
        }
    } catch (std::exception& e) {
        code = reportFailure(e);
    }

    msg::finish(code == 0 ? "ok" : "error", code);
    return code;
}

int Application::runMigrate()
{
    ScopedContext ctx(Diag::ConfigMigrate, "Could not migrate the configuration.");

    const fs::path file = projectFile();
    return config::migrate(file, m_ctx.paths.workDir, m_cli.migrateWrite) ?
        exitValue(ExitCode::Ok) : exitValue(ExitCode::Config);
}

int Application::runConfigPath()
{
    // Deliberately on stdout with nothing around it, so that
    // `ncpatcher config path` can be used in a shell substitution.
    std::cout << projectFile().string() << std::endl;
    return exitValue(ExitCode::Ok);
}

int Application::runConfigValidate()
{
    loadConfigurations();

    {
        ScopedContext ctx(Diag::TargetConfigLoad, "Could not load the target configuration.");
        config::loadTargets(m_config);
    }

    resolveRomDir();

    // Resolving is the validation: inheritance, ${...} expansion and glob
    // matching all report through the same errors a build would raise.
    for (bool arm9 : { false, true })
    {
        if (!m_config.target(arm9).enabled)
            continue;
        Context targetCtx = m_ctx;
        (void)resolveTarget(arm9, targetCtx);
    }

    if (!msg::isJson())
        Log::info("The configuration is valid.");
    return exitValue(ExitCode::Ok);
}

int Application::runConfigDump()
{
    loadConfigurations();

    {
        ScopedContext ctx(Diag::TargetConfigLoad, "Could not load the target configuration.");
        config::loadTargets(m_config);
    }

    resolveRomDir();

    std::vector<ResolvedTarget> resolved;
    for (bool arm9 : { false, true })
    {
        const config::TargetConfig& targetConfig = m_config.target(arm9);
        if (!targetConfig.enabled)
        {
            resolved.push_back({ &targetConfig, BuildTarget{} });
            continue;
        }
        Context targetCtx = m_ctx;
        resolved.push_back(resolveTarget(arm9, targetCtx));
    }

    DumpOptions options;
    options.json = m_cli.dumpJson;
    options.explain = m_cli.explain;

    // The dump is the product, so it goes to stdout whole rather than through
    // the log, which may be styled, wrapped or duplicated into a file.
    dumpConfig(std::cout, m_config, m_ctx.paths, resolved, options);
    return exitValue(ExitCode::Ok);
}

int Application::runClean()
{
    ScopedContext ctx(Diag::CleanFailed, "Could not clean the project.");

    loadConfigurations();
    {
        ScopedContext targetCtx(Diag::TargetConfigLoad, "Could not load the target configuration.");
        config::loadTargets(m_config);
    }

    // Needed only for the warning below, but resolved unconditionally so that
    // the warning cannot be the one code path that names an empty directory.
    resolveRomDir();

    std::uintmax_t removed = 0;
    for (bool arm9 : { false, true })
    {
        const config::TargetConfig& targetConfig = m_config.target(arm9);
        if (!targetConfig.enabled)
            continue;

        const fs::path buildDir = m_ctx.paths.work(targetConfig.buildDir.value);
        if (!fs::exists(buildDir))
            continue;

        Log::info("Removing " + buildDir.string());
        removed += fs::remove_all(buildDir);
    }

    if (m_cli.cleanBackups)
    {
        const fs::path backupDir = m_ctx.backupDir();
        if (fs::exists(backupDir))
        {
            // Said before it happens, because afterwards there is nothing left
            // to say it about: the patched binaries in the ROM directory are
            // then the only copy there is.
            Log::out << OWARN << "Deleting the backups leaves the binaries in "
                     << OSTR(m_ctx.paths.romDir.string()) << " patched, with no way back."
                     << OREASONNL << "Run " ANSI_bCYAN "ncpatcher restore" ANSI_RESET
                     << " instead if you wanted the ROM back the way it was." << std::endl;

            Log::info("Removing " + backupDir.string());
            removed += fs::remove_all(backupDir);
        }
    }

    Log::info("Removed " + std::to_string(removed) + " file(s).");
    return exitValue(ExitCode::Ok);
}

int Application::runRestore()
{
    ScopedContext ctx(Diag::RestoreFailed, "Could not restore the ROM binaries.");

    loadConfigurations();
    resolveRomDir();

    const fs::path backupDir = m_ctx.backupDir();
    if (!fs::exists(backupDir))
    {
        Log::info("Nothing to restore: there are no backups.");
        return exitValue(ExitCode::Ok);
    }

    // The backup directory mirrors the ROM directory, one file per binary the
    // patcher has ever touched, so restoring is a copy back along the same
    // relative paths. Everything else in there is bookkeeping.
    static const char* NOT_A_BINARY[] = { "rebuild.json", "rebuild.bin" };

    std::size_t restored = 0;
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(backupDir))
    {
        if (!entry.is_regular_file())
            continue;

        const fs::path relative = fs::relative(entry.path(), backupDir);
        bool skip = false;
        for (const char* name : NOT_A_BINARY)
            skip = skip || relative == name;
        if (skip)
            continue;

        const fs::path destination = m_ctx.paths.rom(relative);
        fs::create_directories(destination.parent_path());
        fs::copy_file(entry.path(), destination, fs::copy_options::overwrite_existing);

        msg::Artifact artifact;
        artifact.kind = "file";
        artifact.action = "restored";
        artifact.name = relative.generic_string();
        artifact.size = static_cast<long long>(fs::file_size(destination));
        msg::artifact(std::move(artifact));

        restored++;
    }

    // Removed rather than kept: a backup is by definition a copy of a pristine
    // file, and once the pristine file is back in place keeping it would let a
    // later build treat an unpatched binary as if it had already been saved.
    fs::remove_all(backupDir);

    Log::info("Restored " + std::to_string(restored) + " file(s) to " + m_ctx.paths.romDir.string() + ".");
    return exitValue(ExitCode::Ok);
}

// Renders a failure as the phase it happened in, the reason, and then any
// enclosing phases. The innermost context is the headline because it is the
// most specific thing that was being attempted.
int Application::reportFailure(const std::exception& e)
{
    const std::vector<DiagContext>& contexts = diagnostics::failureContext();
    const Diag code = contexts.empty() ? Diag::None : contexts.front().code;

    Log::out << OERROR;
    if (!contexts.empty())
        Log::out << diagCode(code) << ": " << contexts.front().description << "\n" << OREASON;
    Log::out << e.what() << std::endl;

    for (std::size_t i = 1; i < contexts.size(); i++)
        Log::out << "        while " << diagCode(contexts[i].code) << ": " << contexts[i].description << std::endl;

    // A configuration error knows the line it is about; everything else does
    // not. Reporting the location is the difference between a caller being able
    // to point at the offending key and having to grep for the message.
    msg::Location location;
    if (const auto* configError = dynamic_cast<const cfg::cfg_error*>(&e))
    {
        location.file = configError->file().string();
        location.line = configError->mark().line;
        location.column = configError->mark().column;
        location.path = configError->nodePath();
    }

    msg::diagnostic(msg::Level::Error, code, Ansi::strip(e.what()), location);

    diagnostics::clearFailureContext();
    return exitValue(exitCodeFor(code));
}

void Application::runBuild()
{
    Log::out << ANSI_bWHITE " ----- Nitro Code Patcher -----" ANSI_RESET << std::endl;

    loadConfigurations();
    validateToolchain();

    resolveRomDir();

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

    Context targetCtx = m_ctx;
    BuildTarget buildTarget = resolveTarget(isArm9, targetCtx).target;

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

        // A hook's own output is for the person watching, so in json mode it
        // must not land in the middle of the event stream.
        std::ostream& hookOutput = msg::isJson() ? std::cerr : std::cout;
        int retcode = Process::start(command.c_str(), m_ctx.paths.workDir, &hookOutput);
        if (retcode != 0) {
            throw ncp::exception("Process returned: " + std::to_string(retcode));
        }
        
        commandIndex++;
    }
}

// The per-target half of resolution, in one place because `config dump` has to
// answer with exactly what a build would have used.
ResolvedTarget Application::resolveTarget(bool isArm9, Context& targetCtx) const
{
    const config::TargetConfig& targetConfig = m_config.target(isArm9);

    // Per-target anchors. A copy, so the two targets cannot see each other's.
    targetCtx.paths.targetWorkDir = targetConfig.workDir.configured() ?
        m_ctx.paths.work(targetConfig.workDir.value) :
        targetConfig.file.parent_path();
    targetCtx.paths.buildDir = m_ctx.paths.work(targetConfig.buildDir.value);

    ScopedContext ctx(Diag::TargetConfigLoad, isArm9 ?
        "Could not resolve the ARM9 target configuration." :
        "Could not resolve the ARM7 target configuration.");

    ResolvedTarget out;
    out.config = &targetConfig;
    out.target = config::TargetResolver::resolve(m_config, targetConfig, targetCtx.paths);
    return out;
}

std::filesystem::path Application::projectFile() const
{
    // -C naming the file itself is accepted as well as -C naming its directory:
    // both spellings turn up in scripts, and guessing wrong would mean an error
    // about a path the caller can see is right there.
    if (!m_cli.projectPath.empty() && fs::is_regular_file(m_cli.projectPath))
        return fs::absolute(m_cli.projectPath);

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

    config::VarOverrides overrides;
    for (const std::string& assignment : m_cli.vars) {
        const std::size_t separator = assignment.find('=');
        if (separator == std::string::npos) {
            std::ostringstream oss;
            oss << "Malformed " << OSTRa("--var") << " argument " << OSTR(assignment) << "."
                << OREASONNL << "Expected " << OSTRa("NAME=VALUE") << ".";
            throw ncp::exception(oss.str());
        }
        overrides.emplace_back(assignment.substr(0, separator), assignment.substr(separator + 1));
    }

    m_config = config::load(projectFile(), m_ctx.paths.workDir, overrides);

    applyCommandLineOverrides();

    m_rebuild.load(m_ctx.backupDir() / "rebuild.json");
}

// Command line, then NCPATCHER_* environment, then the file. Applied after the
// file is read rather than before, so that Setting<T>::source still records
// which of them won and `config dump --explain` can say so.
void Application::applyCommandLineOverrides()
{
    if (m_cli.toolchainSource != config::Source::Default)
        m_config.toolchain.set(m_cli.toolchain, m_cli.toolchainSource);

    if (m_cli.jobsSource != config::Source::Default)
        m_config.threadCount.set(m_cli.jobs, m_cli.jobsSource);

    if (!m_cli.romDir.empty()) {
        m_config.filesystemDir.set(m_cli.romDir, config::Source::CommandLine);
        // --rom names a directory, so it also settles the question the config
        // file would otherwise have answered with rom.file.
        m_config.romFile = {};
    }
}

// Turns the configured extracted-ROM directory into the absolute anchor that
// every binary in the ROM is named relative to.
void Application::resolveRomDir()
{
    if (m_config.romFile.configured()) {
        std::ostringstream oss;
        oss << "Reading a ROM file directly is not supported by this version of NCPatcher."
            << OREASONNL << "Extract the ROM and point " << OSTRa("rom.dir") << " at the result.";
        throw ncp::exception(oss.str());
    }

    m_ctx.paths.romDir = m_ctx.paths.work(m_config.filesystemDir.value);
}

void Application::saveRebuildConfig()
{
    m_rebuild.setProjectHash(
        Hash::of(config::TargetResolver::describeProject(m_config, m_options.defines)));
    m_rebuild.save(m_ctx.backupDir() / "rebuild.json");
}

void Application::validateToolchain()
{
    ScopedContext ctx(Diag::ToolchainMissing, "The build toolchain is not usable.");

    const std::string& toolchain = m_ctx.toolchain();
    std::string gccPath = toolchain + "gcc";

    if (!Process::exists(gccPath.c_str())) {
        std::ostringstream oss;
        oss << "The building toolchain " << OSTR(toolchain) << " was not found." << OREASONNL;
        oss << "Make sure that it is correctly specified in "
            << OSTR(m_config.file.filename().string())
            << ", or override it with " << OSTRa("--toolchain") << " or "
            << OSTRa("NCPATCHER_TOOLCHAIN") << ", and that it is present on your system.";
        throw ncp::exception(oss.str());
    }
}

void Application::initializePaths()
{
    // The cwd is read exactly once, here, and is never written. Everything
    // downstream resolves against m_ctx.paths instead.
    m_ctx.paths.appDir = fetchAppPath();

    // -C is what removes the "must be launched from the project directory"
    // constraint that the level editor and CTGPNitro's build script each work
    // around by chdir-ing first.
    if (m_cli.projectPath.empty()) {
        m_ctx.paths.workDir = fs::current_path();
    } else if (fs::is_regular_file(m_cli.projectPath)) {
        m_ctx.paths.workDir = fs::absolute(m_cli.projectPath).parent_path();
    } else {
        m_ctx.paths.workDir = fs::absolute(m_cli.projectPath);
    }
}

void Application::initializeLogging()
{
    if (m_cli.noLog)
        return;

    Log::openLogFile(m_cli.logPathSet ? m_cli.logPath : m_ctx.paths.appDir / "log.txt");
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

} // namespace ncp
