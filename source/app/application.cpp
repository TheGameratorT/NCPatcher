#include "application.hpp"

#include <fstream>
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
#include "../system/paths.hpp"
#include "../utils/types.hpp"
#include "../utils/json.hpp"
#include "../config/buildtarget.hpp"
#include "../config/config_loader.hpp"
#include "../config/migrate.hpp"
#include "../config/target_resolver.hpp"
#include "../modules/module_resolver.hpp"
#include "../modules/module_dump.hpp"
#include "config_dump.hpp"
#include "../utils/hash.hpp"
#include "../rom/dir_accessor.hpp"
#include "../rom/nds_accessor.hpp"
#include "../rom/backup_store.hpp"
#include "rom_command.hpp"
#include "../build/objmaker.hpp"
#include "../patch/patch_maker.hpp"
#include "../core/compilation_unit_manager.hpp"

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
		m_cli.command == Command::ConfigPath ||
		m_cli.command == Command::ModulesList ||
		m_cli.command == Command::ModulesDump ||
		m_cli.command == Command::ModulesExplain;
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
		case Command::ModulesList:
		case Command::ModulesDump:
		case Command::ModulesExplain: code = runModulesCommand(); break;
		case Command::RomInfo:
		case Command::RomExtract:
		case Command::RomPack:        code = runRomCommand(); break;
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

int Application::runModulesCommand()
{
	ScopedContext ctx(Diag::ModuleResolve, "Could not inspect the modules.");

	loadConfigurations();

	// `modules dump` is a pipe on the far end of somebody's build script, so the
	// graph it prints must be the whole of standard output. The other two are
	// for a person, and go there too for consistency.
	loadModules(m_cli.command == Command::ModulesDump);

	if (!m_config.modules.present)
	{
		std::ostringstream oss;
		oss << "This project has no " << OSTRa("modules") << " section."
			<< OREASONNL "Add one to " << OSTR(m_config.file.filename().string())
			<< " to use the module system.";
		throw ncp::exception(oss.str());
	}

	switch (m_cli.command)
	{
	case Command::ModulesList:
		modules::writeList(std::cout, m_modules);
		break;

	case Command::ModulesDump:
		if (m_cli.modulesOutPath.empty())
		{
			modules::writeDump(std::cout, m_modules);
		}
		else
		{
			const fs::path path = m_ctx.paths.work(m_cli.modulesOutPath);
			std::error_code error;
			fs::create_directories(path.parent_path(), error);

			std::ofstream file(path);
			if (!file.is_open())
			{
				std::ostringstream oss;
				oss << "Could not open " << OSTR(path.string()) << " for writing.";
				throw ncp::exception(oss.str());
			}
			modules::writeDump(file, m_modules);
		}
		break;

	default:
		modules::writeExplanation(std::cout, m_modules, m_cli.modulesTarget);
		break;
	}

	return exitValue(ExitCode::Ok);
}

int Application::runRomCommand()
{
	ScopedContext ctx(Diag::RomAccess, "Could not read the ROM.");

	const fs::path target = romTarget();
	const rom::DirLayout layout = romLayout();

	switch (m_cli.command)
	{
	case Command::RomInfo:
		romcmd::info(target, layout);
		break;
	case Command::RomExtract:
		romcmd::extract(target, fs::absolute(m_cli.romDirArgument), layout);
		break;
	case Command::RomPack:
		romcmd::pack(target, fs::absolute(m_cli.romDirArgument),
					 m_cli.outPath.empty() ? fs::path() : fs::absolute(m_cli.outPath),
					 layout,
					 m_config.romArm9Slack.configured() ? m_config.romArm9Slack.value : DEFAULT_ARM9_SLACK);
		break;
	default:
		break;
	}

	return exitValue(ExitCode::Ok);
}

fs::path Application::romTarget()
{
	if (!m_cli.romPath.empty())
		return fs::absolute(m_cli.romPath);

	loadConfigurations();
	resolveRomDir();

	if (!m_romFile.empty())
		return m_romFile;

	if (m_cli.command == Command::RomInfo)
		return m_ctx.paths.romDir;

	std::ostringstream oss;
	oss << "This project patches an extracted directory, and there is no ROM to "
		<< (m_cli.command == Command::RomExtract ? "extract from" : "pack into") << "." << OREASONNL
		<< "Give " << OSTRa("--rom") << " a .nds, or set " << OSTRa("rom.file") << " in the project.";
	throw ncp::exception(oss.str());
}

rom::DirLayout Application::romLayout() const
{
	rom::DirLayout layout;
	const std::string presetName = m_config.romLayoutPreset.configured()
		? m_config.romLayoutPreset.value : std::string("ncpatcher");
	if (!rom::DirLayout::preset(presetName, layout))
	{
		std::ostringstream oss;
		oss << "Unknown ROM directory layout " << OSTR(presetName) << "." << OREASONNL
			<< "Known layouts: " << rom::DirLayout::presetNames() << ".";
		throw ncp::exception(oss.str());
	}

	for (const auto& [key, value] : m_config.romLayoutOverrides)
	{
		if (!rom::DirLayout::setField(layout, key, value))
		{
			std::ostringstream oss;
			oss << "Unknown ROM layout file " << OSTRa(key) << "." << OREASONNL
				<< "Known names: " << rom::DirLayout::fieldNames() << ".";
			throw ncp::exception(oss.str());
		}
	}
	return layout;
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
	loadModules();

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
	loadModules(true);

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
	loadModules(true);
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

	if (!m_romFile.empty() && m_config.romOutput.configured())
	{
		// With an output file the source ROM is never written to, so there is
		// nothing in it to put back. The backups still go, because they are
		// what makes the next build start from a pristine binary and keeping
		// them would be keeping half a restore.
		Log::info("Nothing to restore: this project writes a patched copy and leaves "
				  + m_romFile.string() + " alone.");
		fs::remove_all(backupDir);
		return exitValue(ExitCode::Ok);
	}

	// The backup directory records, by its own contents, every binary the
	// patcher has ever touched. Restoring writes each one back through the ROM
	// accessor rather than copying files into place, so that a project which
	// patches a .nds is restorable too -- and so that a project using a
	// non-default extracted layout gets its own file names back.
	static const char* NOT_A_BINARY[] = { "rebuild.json", "rebuild.bin" };

	std::unique_ptr<rom::RomAccessor> rom = openRom();
	const rom::BackupStore backup(backupDir);

	std::size_t restored = 0;
	for (const fs::directory_entry& entry : fs::recursive_directory_iterator(backupDir))
	{
		if (!entry.is_regular_file())
			continue;

		const std::string key = fs::relative(entry.path(), backupDir).generic_string();
		bool skip = false;
		for (const char* name : NOT_A_BINARY)
			skip = skip || key == name;
		if (skip)
			continue;

		using Kind = rom::BackupStore::Key::Kind;
		const rom::BackupStore::Key parsed = rom::BackupStore::classify(key);
		if (parsed.kind == Kind::Unknown)
		{
			Log::out << OWARN << "Not restoring " << OSTR(key)
					 << ": nothing in a ROM goes by that name." << std::endl;
			continue;
		}

		const std::vector<u8> bytes = backup.read(key);
		std::string name;
		switch (parsed.kind)
		{
		case Kind::Arm:
			rom->writeArm(parsed.arm9, bytes);
			name = rom->nameOfArm(parsed.arm9);
			break;
		case Kind::OverlayTable:
			rom->writeOverlayTable(parsed.arm9, rom::OverlayTable::parse(bytes));
			name = rom->nameOfOverlayTable(parsed.arm9);
			break;
		case Kind::Overlay:
			rom->writeOverlay(parsed.arm9, parsed.overlayId, bytes);
			name = rom->nameOfOverlay(parsed.arm9, parsed.overlayId);
			break;
		case Kind::Unknown:
			break;
		}

		msg::Artifact artifact;
		artifact.kind = "file";
		artifact.action = "restored";
		artifact.name = name;
		artifact.size = static_cast<long long>(bytes.size());
		msg::artifact(std::move(artifact));

		restored++;
	}

	rom->commit();

	// Removed rather than kept: a backup is by definition a copy of a pristine
	// file, and once the pristine file is back in place keeping it would let a
	// later build treat an unpatched binary as if it had already been saved.
	fs::remove_all(backupDir);

	Log::info("Restored " + std::to_string(restored) + " binaries to " + rom->location().string() + ".");
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
	openDefaultLogFile();
	validateToolchain();

	resolveRomDir();

	std::unique_ptr<rom::RomAccessor> rom = openRom();

	loadModules();
	writeModuleDump();

	runCommandList(m_config.preBuild,
				   "Running pre-build commands...",
				   Diag::PreBuildCommand,
				   "Not all pre-build commands succeeded.");

	// Only now: a v1 project may have just generated its target files.
	{
		ScopedContext ctx(Diag::TargetConfigLoad, "Could not load the target configuration.");
		config::loadTargets(m_config);
	}
	openDefaultLogFile();

	if (m_config.arm7.enabled) {
		processTarget(*rom, false); // ARM7
	}

	if (m_config.arm9.enabled) {
		processTarget(*rom, true);  // ARM9
	}

	// After both targets: the container backend holds its writes until here, so
	// a build that fails half way through never leaves a ROM with one
	// processor's code in it and not the other's.
	rom->commit();

	saveRebuildConfig();

	runCommandList(m_config.postBuild,
				   "Running post-build commands...",
				   Diag::PostBuildCommand,
				   "Not all post-build commands succeeded.");

	Log::info("All tasks finished.");
}

void Application::processTarget(rom::RomAccessor& rom, bool isArm9)
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
	patchMaker.makeTarget(buildTarget, targetCtx, rom, compilationUnitsMgr);

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
	out.target = config::TargetResolver::resolve(m_config, targetConfig, targetCtx.paths, &m_modules);
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

void Application::loadModules(bool quiet)
{
	if (!m_config.modules.present)
		return;

	ScopedContext ctx(Diag::ModuleResolve, "Could not resolve the modules.");

	modules::ResolveOptions options;
	options.quiet = quiet;
	m_modules = modules::resolve(m_config.modules, m_ctx.paths.workDir, options);
}

// Writes the graph where the project asked for it.
//
// Before the pre-build commands rather than after, because the whole point of
// the dump is that a generator which turns modules into game-specific headers
// is an ordinary hook rather than something this program has to host.
void Application::writeModuleDump()
{
	if (!m_config.modules.dump.configured() || !m_config.modules.present)
		return;

	ScopedContext ctx(Diag::ModuleResolve, "Could not write the module dump.");

	const fs::path path = m_ctx.paths.work(m_config.modules.dump.value);

	std::error_code error;
	fs::create_directories(path.parent_path(), error);

	std::ofstream file(path);
	if (!file.is_open())
	{
		std::ostringstream oss;
		oss << "Could not open " << OSTR(path.string()) << " for writing.";
		throw ncp::exception(oss.str());
	}

	modules::writeDump(file, m_modules);
	Log::info("Wrote the module dump.");
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

	if (!m_cli.romPath.empty()) {
		// --rom settles which of rom.file and rom.dir applies, so whichever it
		// did not name is cleared rather than left to contradict it.
		if (looksLikeRomFile(m_cli.romPath)) {
			m_config.romFile.set(m_cli.romPath, config::Source::CommandLine);
			m_config.filesystemDir = {};
		} else {
			m_config.filesystemDir.set(m_cli.romPath, config::Source::CommandLine);
			m_config.romFile = {};
		}
	}

	if (!m_cli.outPath.empty())
		m_config.romOutput.set(m_cli.outPath, config::Source::CommandLine);
}

// Decides whether a path names a ROM image or a directory holding an extracted
// one. Checked against the filesystem first, because that is the only answer
// that cannot be wrong; the extension only has to settle the case where the
// path does not exist yet, which for an output it usually does not.
bool Application::looksLikeRomFile(const fs::path& path)
{
	std::error_code error;
	if (fs::is_directory(path, error))
		return false;
	if (fs::is_regular_file(path, error))
		return true;

	std::string extension = path.extension().string();
	for (char& c : extension)
		c = char(std::tolower(static_cast<unsigned char>(c)));
	return extension == ".nds" || extension == ".dsi" || extension == ".srl";
}

// Turns the configured ROM location into the absolute anchor that every binary
// in the ROM is named relative to.
//
// For a .nds that anchor is the directory the file sits in. Nothing is read
// relative to it in that mode, but it is what ${rom.dir} expands to and what
// messages name, so it still has to be a real directory rather than empty.
void Application::resolveRomDir()
{
	// The project and the command line can contradict each other here, which is
	// a configuration problem rather than an unanticipated one -- and the exit
	// code has to say so.
	ScopedContext ctx(Diag::ConfigLoad, "The ROM to patch is not settled.");

	if (m_config.romFile.configured()) {
		m_romFile = m_ctx.paths.work(m_config.romFile.value);
		m_ctx.paths.romDir = m_romFile.parent_path();
		return;
	}

	m_romFile.clear();
	m_ctx.paths.romDir = m_ctx.paths.work(m_config.filesystemDir.value);

	if (m_config.romOutput.configured()) {
		std::ostringstream oss;
		oss << OSTRa("--out") << " writes a patched ROM file, and this project patches an "
			<< "extracted directory." << OREASONNL
			<< "Point " << OSTRa("rom.file") << " or " << OSTRa("--rom") << " at a .nds to use it.";
		throw ncp::exception(oss.str());
	}
}

std::unique_ptr<rom::RomAccessor> Application::openRom()
{
	ScopedContext ctx(Diag::RomAccess, "Could not open the ROM.");

	if (!m_romFile.empty())
	{
		auto accessor = std::make_unique<rom::NdsRomAccessor>(
			m_romFile,
			m_config.romOutput.configured() ? m_ctx.paths.work(m_config.romOutput.value) : fs::path(),
			m_config.romArm9Slack.configured() ? m_config.romArm9Slack.value : DEFAULT_ARM9_SLACK);
		accessor->loadRom();
		return accessor;
	}

	auto accessor = std::make_unique<rom::DirRomAccessor>(m_ctx.paths.romDir, romLayout());
	accessor->loadHeader();
	return accessor;
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
	// The cwd is read exactly once, here, and is never written; everything
	// downstream resolves against m_ctx.paths instead. -C is what removes the
	// "must be launched from the project directory" constraint that the level
	// editor and CTGPNitro's build script each work around by chdir-ing first.
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

	// An explicit path is a path; it can be opened now. The default one cannot:
	// it lives in the build directory, which is something the configuration
	// answers, so until then the log is held in memory. Commands other than a
	// build get no log file unless one was asked for -- `config dump` and
	// `clean` have no build directory of their own to write into, and creating
	// one as a side effect of printing is not a trade worth making.
	if (m_cli.logPathSet)
		Log::openLogFile(m_cli.logPath);
	else if (m_cli.command == Command::Build)
		Log::beginBufferedLogFile();
}

// Where <buildDir>/ncpatcher.log goes when nothing named a path.
//
// The ARM9 target's build directory, or the only enabled target's. Not the
// directory the two share: nsmb-coop builds into code/build and code/build7,
// whose common parent is code/ -- the source tree. A log is a build artifact,
// so it belongs somewhere `clean` will take it away again.
std::filesystem::path Application::logDirectory() const
{
	for (bool arm9 : { true, false })
	{
		const config::TargetConfig& targetConfig = m_config.target(arm9);
		if (targetConfig.enabled && targetConfig.buildDir.configured())
			return m_ctx.paths.work(targetConfig.buildDir.value);
	}
	return {};
}

// Called once the build directory is known, and again after a v1 project's
// target files have been read, since those may not have existed until a
// pre-build command wrote them. Doing nothing when a log is already open is
// what makes calling it twice correct.
void Application::openDefaultLogFile()
{
	if (m_cli.noLog || m_cli.logPathSet || Log::logFileOpen())
		return;

	const fs::path dir = logDirectory();
	if (dir.empty())
		return;

	try {
		std::error_code error;
		fs::create_directories(dir, error);
		Log::openLogFile(dir / "ncpatcher.log");
	} catch (const std::exception& ex) {
		// Not fatal: the console still has everything, and failing a build
		// because its log file could not be opened would be a poor trade.
		Log::out << OWARN << "Could not open the log file in " << OSTR(dir.string())
				 << "." << OREASONNL << ex.what() << std::endl;
	}
}

} // namespace ncp
