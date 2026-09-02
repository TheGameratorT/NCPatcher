#include "application.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <tuple>

#include "../system/ansi.hpp"
#include "../system/log.hpp"
#include "../system/process.hpp"
#include "../system/except.hpp"
#include "../system/cache.hpp"
#include "../system/cancel.hpp"
#include "../system/diagnostics.hpp"
#include "../system/exit_code.hpp"
#include "../system/message.hpp"
#include "../system/paths.hpp"
#include "../utils/types.hpp"
#include "../utils/unicode.hpp"
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
#include "../rom/file_tree.hpp"
#include "../rom/file_manifest.hpp"
#include "../rom/plan_accessor.hpp"
#include "../rom/narc.hpp"
#include "rom_command.hpp"
#include "project_init.hpp"
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
	// whose product *is* stdout, since a `config dump` with a deprecation warning
	// mixed into it is not something a caller can pipe anywhere.
	//
	// The rom reporting commands belong to the second group for the same
	// reason. `rom files --json` writes `ncpatcher.files/1`, and a parser
	// reading it cannot be asked to skip whatever the loader had to say first.
	const bool logToStderr =
		m_cli.messageFormat == msg::Format::Json ||
		m_cli.command == Command::ConfigDump ||
		m_cli.command == Command::ConfigPath ||
		m_cli.command == Command::ModulesList ||
		m_cli.command == Command::ModulesDump ||
		m_cli.command == Command::ModulesExplain ||
		m_cli.command == Command::RomInfo ||
		m_cli.command == Command::RomFiles ||
		m_cli.command == Command::FilesPlan;
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

	// After logging, so a cancellation has somewhere to be reported, and before
	// anything long-running, which is everything below.
	cancel::install();

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
		case Command::Init:           code = runInit(); break;
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
		case Command::RomFiles:
		case Command::RomExtract:
		case Command::RomPack:        code = runRomCommand(); break;
		case Command::FilesPlan:      code = runFilesPlan(); break;
		default:                      runBuild(); break;
		}
	} catch (const ncp::cancelled&) {
		// Not a failure, and reported as one it would be: nothing is wrong with
		// the project, nothing has to be fixed, and the next run picks up from
		// the pristine binaries the backup store holds.
		Log::info("Cancelled.");
		code = exitValue(ExitCode::Cancelled);
	} catch (std::exception& e) {
		code = reportFailure(e);
	}

	// The result document is written either way, so a caller that asked for one
	// learns what had finished before the stop rather than nothing at all.
	msg::finish(code == 0 ? "ok" : (code == exitValue(ExitCode::Cancelled) ? "cancelled" : "error"),
		code);
	return code;
}

int Application::runInit()
{
	ScopedContext ctx(Diag::ProjectInit, "Could not initialize the project.");
	project::initialize(m_ctx.paths.workDir, m_cli.initTemplate);
	Log::info("Initialized the " + m_cli.initTemplate + " project in " + m_ctx.paths.workDir.string() + ".");
	return exitValue(ExitCode::Ok);
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
		romcmd::info(std::cout, target, layout);
		break;
	case Command::RomFiles:
		romcmd::files(std::cout, target, layout, m_cli.dumpJson);
		break;
	case Command::RomExtract:
	{
		romcmd::ExtractOptions options;
		options.codeOnly = m_cli.extractCodeOnly;
		options.decompressOverlays = m_cli.extractDecompressOverlays;
		romcmd::extract(target, fs::absolute(m_cli.romDirArgument), layout, options);
		break;
	}
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

// `files plan`: what a build would place in the ROM's file table, without a
// build having to happen first.
//
// An editor showing "this file is pending" has to know which id a file it has
// not yet built will be given, and until now the only way to find out was to
// build. The answer comes from running the real insertion pass against a ROM
// accessor that writes nothing (see plan_accessor.hpp), so a prediction and the
// build that confirms it come out of the same code rather than out of two
// implementations of the same rules.
int Application::runFilesPlan()
{
	loadConfigurations();

	if (m_cli.variant.empty())
	{
		// The same refusal `build` makes, for the same reason: which file a
		// tree contributes depends on the variant, so there is no variant-less
		// answer that any build would ever confirm.
		if (!m_config.variants.empty())
		{
			ScopedContext variantCtx(Diag::ConfigLoad, "Could not select the variant to plan for.");
			throw ncp::exception("This project has variants; choose one with "
				ANSI_bCYAN "--variant NAME" ANSI_RESET "."
				OREASONNL "Which files a build places depends on the variant it builds.");
		}
	}
	else
	{
		applyVariant(m_cli.variant, false);
	}

	ScopedContext ctx(Diag::NitroFsInsert, "Could not plan the NitroFS files.");

	resolveRomDir();
	std::unique_ptr<rom::RomAccessor> base = openRom();

	// Quiet, because standard output is the document when --json is given and
	// the module listing is for a person.
	loadModules(true);

	rom::PlanRomAccessor plan(*base);
	const rom::InsertionRecord inserted = insertFiles(plan, true);
	const std::vector<rom::ManifestEntry> entries =
		rom::buildManifest(plan, inserted, m_ctx.paths.workDir);

	std::ofstream file;
	std::ostream* out = &std::cout;
	if (!m_cli.filesOutPath.empty())
	{
		const fs::path path = fs::absolute(m_cli.filesOutPath);
		std::error_code error;
		fs::create_directories(path.parent_path(), error);
		file.open(path);
		if (!file.is_open())
		{
			std::ostringstream oss;
			oss << "Could not open " << OSTR(path.string()) << " for writing.";
			throw ncp::exception(oss.str());
		}
		out = &file;
	}

	if (m_cli.dumpJson)
	{
		rom::writeManifest(*out, entries, m_variant, true);
		out->flush();
		return exitValue(ExitCode::Ok);
	}

	std::size_t changed = 0;
	for (const rom::ManifestEntry& entry : entries)
	{
		if (entry.action == rom::FileAction::Unchanged)
			continue;
		changed++;

		const char* action = entry.action == rom::FileAction::Created ? "create" : "modify";
		*out << std::setw(5) << entry.id << "  " << action << "  "
		     << std::setw(9) << entry.size << "  " << entry.path;
		if (!entry.module.empty())
			*out << "  <- " << entry.module;
		if (!entry.fromVariant.empty())
			*out << " (" << entry.fromVariant << ")";
		if (entry.sourceMissing)
			*out << "  [source not generated yet]";
		*out << '\n';
	}
	*out << changed << " of " << entries.size() << " file(s) would change.\n";
	out->flush();
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

	// Both read an extracted directory perfectly well; the two that move code
	// binaries between a .nds and a directory are the ones that need a .nds.
	if (m_cli.command == Command::RomInfo || m_cli.command == Command::RomFiles)
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
		config::loadTargets(m_config, targetLoadOptions());
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
		config::loadTargets(m_config, targetLoadOptions());
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
		config::loadTargets(m_config, targetLoadOptions());
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
	// patches a .nds is restorable too, and so that a project using a
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

	if (m_cli.allVariants)
	{
		if (m_config.variants.empty())
		{
			ScopedContext ctx(Diag::ConfigLoad, "Could not select the build variants.");
			throw ncp::exception("This project does not configure any variants.");
		}

		const config::ProjectConfig base = m_config;
		for (const config::VariantConfig& variant : base.variants)
		{
			m_config = base;
			m_modules = {};
			applyVariant(variant.name, true);
			Log::out << ANSI_bWHITE " ----- Variant: " << variant.name << " -----" ANSI_RESET << std::endl;
			runConfiguredBuild();
		}
		return;
	}

	if (m_cli.variant.empty())
	{
		if (!m_config.variants.empty())
		{
			ScopedContext ctx(Diag::ConfigLoad, "Could not select the build variant.");
			throw ncp::exception("This project has variants; choose one with --variant NAME or build all with --all-variants.");
		}
	}
	else
	{
		applyVariant(m_cli.variant, false);
	}

	runConfiguredBuild();
}

void Application::runConfiguredBuild()
{
	openDefaultLogFile();

	// Only when there is something to compile. An assets-only project has no
	// use for a cross compiler, and demanding one is how "I just want to
	// replace a texture" turns into installing a toolchain.
	if (m_config.arm7.enabled || m_config.arm9.enabled)
		validateToolchain();

	resolveRomDir();

	std::unique_ptr<rom::RomAccessor> rom = openRom();

	loadModules();
	writeModuleDump();
	refuseUnbuildableModuleCode();

	// Between phases, which is where stopping leaves nothing half-done. See
	// system/cancel.hpp for why these are chosen rather than sprinkled.
	cancel::checkpoint();

	runHooks(config::HookWhen::PreBuild,
			 "Running pre-build hooks...",
			 Diag::PreBuildCommand,
			 "Not all pre-build hooks succeeded.");

	// Hooks may generate the source files. Insert them after hooks but before
	// target resolution and compilation, so every generated file id is settled
	// before code which refers to it is built.
	cancel::checkpoint();

	const rom::InsertionRecord inserted = insertFiles(*rom);
	insertBanner(*rom);

	// Before the post-files hooks: a generator that turns file ids into
	// constants is exactly what that phase is for, and it has to be able to
	// read them.
	writeFileDump(*rom, inserted);

	runHooks(config::HookWhen::PostFiles,
			 "Running post-files hooks...",
			 Diag::PostFilesCommand,
			 "Not all post-files hooks succeeded.");

	// Only now: a v1 project may have just generated its target files.
	{
		ScopedContext ctx(Diag::TargetConfigLoad, "Could not load the target configuration.");
		config::loadTargets(m_config, targetLoadOptions());
	}
	openDefaultLogFile();

	cancel::checkpoint();

	if (m_config.arm7.enabled) {
		processTarget(*rom, false); // ARM7
	}

	if (m_config.arm9.enabled) {
		processTarget(*rom, true);  // ARM9
	}

	// The last one. Past here the ROM is written, and a build that has produced
	// its output has finished whatever anyone asked of it since.
	cancel::checkpoint();

	// After both targets: the container backend holds its writes until here, so
	// a build that fails half way through never leaves a ROM with one
	// processor's code in it and not the other's.
	rom->commit();

	saveRebuildConfig();

	runHooks(config::HookWhen::PostBuild,
			 "Running post-build hooks...",
			 Diag::PostBuildCommand,
			 "Not all post-build hooks succeeded.");

	Log::info("All tasks finished.");
}

// A module's code reaches the ROM by being folded into a target's regions, so
// with no target enabled there is nowhere for it to land. Dropping it silently
// is the failure this exists to prevent: the build succeeds, the ROM boots, and
// the patch is simply not in it.
void Application::refuseUnbuildableModuleCode() const
{
	if (m_config.arm7.enabled || m_config.arm9.enabled)
		return;

	for (bool arm9 : { false, true })
	{
		if (m_modules.contribution(arm9).regionSources.empty())
			continue;

		ScopedContext ctx(Diag::ConfigLoad, "Could not resolve the build configuration.");
		std::ostringstream oss;
		oss << "The enabled modules have " << (arm9 ? "ARM9" : "ARM7")
		    << " code, but this project declares no " << (arm9 ? "arm9" : "arm7")
		    << " target to build it into." OREASONNL
		    << "Declare one under " << OSTRa("targets")
		    << ", or enable only the modules whose assets this project wants.";
		throw ncp::exception(oss.str());
	}
}

void Application::applyVariant(const std::string& name, bool deriveOutput)
{
	ScopedContext ctx(Diag::ConfigLoad, "Could not select the build variant.");

	m_variant = name;

	const auto found = std::find_if(m_config.variants.begin(), m_config.variants.end(),
		[&](const config::VariantConfig& variant) { return variant.name == name; });
	if (found == m_config.variants.end())
	{
		std::ostringstream oss;
		oss << "Unknown variant " << OSTR(name) << ".";
		if (!m_config.variants.empty())
		{
			oss << OREASONNL "Expected one of: ";
			for (std::size_t i = 0; i < m_config.variants.size(); i++)
				oss << (i == 0 ? "" : ", ") << m_config.variants[i].name;
		}
		throw ncp::exception(oss.str());
	}

	for (const config::FileConfig& file : found->files)
	{
		const auto existing = std::find_if(m_config.files.begin(), m_config.files.end(),
			[&](const config::FileConfig& base) { return base.path == file.path; });
		if (existing == m_config.files.end())
			m_config.files.push_back(file);
		else
			*existing = file;
	}

	// Command-line defines retain their normal highest precedence by coming
	// after the selected variant's defines on the compiler command line.
	m_options.defines = found->defines;
	m_options.defines.insert(m_options.defines.end(), m_cli.defines.begin(), m_cli.defines.end());

	if (!deriveOutput)
		return;
	if (!m_config.romFile.configured())
		throw ncp::exception("--all-variants needs rom.file; an extracted directory has only one in-place output.");

	const fs::path base = m_config.romOutput.configured()
		? m_config.romOutput.value : m_config.romFile.value;
	const fs::path output = base.parent_path()
		/ (base.stem().string() + "_" + found->name + base.extension().string());
	const config::Source source = m_config.romOutput.configured()
		? m_config.romOutput.source : m_config.romFile.source;
	m_config.romOutput.set(output, source);
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

// Finishes the expansion the configuration reader deliberately left alone.
//
// `${variant.name}` and `${rom.output}` are not knowable when the file is read:
// the variant has not been chosen and --out has not been applied. The reader
// registers them as deferred, so they survive as literal text, and this
// substitutes them when a hook actually runs. Only those exact tokens are
// touched, so this is not a second general expansion pass over the text.
std::string Application::resolveDeferred(std::string text) const
{
	const fs::path outputPath = m_config.romOutput.configured()
		? m_ctx.paths.work(m_config.romOutput.value)
		: (m_config.romFile.configured() ? m_ctx.paths.work(m_config.romFile.value) : fs::path());

	const std::pair<std::string_view, std::string> replacements[] = {
		{ "${variant.name}", m_variant },
		{ "${rom.output}", outputPath.string() },
	};

	for (const auto& [token, value] : replacements)
	{
		for (std::size_t pos = text.find(token); pos != std::string::npos;
		     pos = text.find(token, pos + value.size()))
		{
			text.replace(pos, token.size(), value);
		}
	}
	return text;
}

void Application::runHooks(config::HookWhen when,
						   const char* message,
						   Diag code,
						   const char* errorContext)
{
	const bool any = std::any_of(m_config.hooks.begin(), m_config.hooks.end(),
		[when](const config::HookConfig& hook) { return hook.when == when; });
	if (!any) {
		return;
	}

	Log::info(message);
	ScopedContext ctx(code, errorContext);

	for (const config::HookConfig& hook : m_config.hooks) {
		if (hook.when != when)
			continue;

		const std::string run = resolveDeferred(hook.run);

		std::ostringstream oss;
		oss << ANSI_bWHITE "[" << hook.name << "] " ANSI_bYELLOW << run << ANSI_RESET;
		Log::info(oss.str());

		// A hook's own output is for the person watching, so in json mode it
		// must not land in the middle of the event stream.
		std::ostream& hookOutput = msg::isJson() ? std::cerr : std::cout;
		const fs::path cwd = hook.cwd.empty()
			? m_ctx.paths.workDir
			: m_ctx.paths.work(utf8ToPath(resolveDeferred(pathToUtf8(hook.cwd))));

		// The project's .ncpatcher.env describes the environment this project
		// builds in, so a hook builds in it too: a generator that resolves the
		// same variable would otherwise read the stale ambient one and disagree
		// with the configuration it was handed. The hook's own `env:` comes
		// after, and therefore wins.
		std::vector<std::pair<std::string, std::string>> env = m_envFile.entries();
		for (const auto& [name, value] : hook.env)
			env.emplace_back(name, resolveDeferred(value));

		int retcode = Process::start(run.c_str(), cwd, env, &hookOutput);
		if (retcode != 0) {
			throw ncp::exception("Hook \"" + hook.name + "\" returned: " + std::to_string(retcode));
		}
	}
}

std::vector<config::FileConfig> Application::resolveNitroFiles(rom::RomAccessor& rom) const
{
	std::vector<rom::FileTree> trees;

	for (const config::FileTreeConfig& declared : m_config.fileTrees)
	{
		rom::FileTree tree;
		tree.dir = m_ctx.paths.work(declared.dir);
		tree.layered = declared.layered;
		tree.baseVariant = declared.baseVariant;
		tree.into = declared.into;
		tree.origin = "the project";
		trees.push_back(std::move(tree));
	}

	// The layer mapping belongs to the variant being built, if any.
	const std::vector<std::pair<std::string, std::string>>* mapped = nullptr;
	if (!m_variant.empty())
	{
		const auto variant = std::find_if(m_config.variants.begin(), m_config.variants.end(),
			[&](const config::VariantConfig& candidate) { return candidate.name == m_variant; });
		if (variant != m_config.variants.end())
			mapped = &variant->moduleVariants;
	}

	// Modules contribute in `modules.enabled` order, which is the order the
	// graph resolved them in, so a conflict message names them the way the
	// project lists them.
	for (const modules::ResolvedModule& module : m_modules.modules())
	{
		if (module.def == nullptr || !module.def->nitrofs.declared)
			continue;

		const modules::NitroFsDef& declared = module.def->nitrofs;

		rom::FileTree tree;
		tree.dir = module.dir / fs::path(declared.dir);
		tree.layered = declared.layered;
		tree.baseVariant = declared.baseVariant;
		tree.into = declared.into;
		tree.module = module.key;
		tree.origin = "module " + module.key;

		if (mapped != nullptr)
		{
			const auto found = std::find_if(mapped->begin(), mapped->end(),
				[&](const auto& entry) { return entry.first == module.key || entry.first == module.id; });
			if (found != mapped->end())
			{
				if (!declared.layered)
				{
					throw ncp::exception("Variant \"" + m_variant + "\" maps module \"" + module.key
						+ "\" to \"" + found->second + "\", but that module's NitroFS tree is not "
						ANSI_bCYAN "layered" ANSI_RESET "."
						OREASONNL "An unlayered tree contributes the same files to every variant.");
				}
				tree.variantLayer = found->second;
			}
		}

		// A disabled component subtracts its own patterns; an enabled one only
		// labels what it owns. This is the single place the component `files:`
		// key is acted on.
		for (const modules::ResolvedComponent& component : module.components)
		{
			if (component.files.empty())
				continue;
			tree.components.push_back(rom::FileTree::Component{
				component.name, component.enabled, component.files });
		}

		trees.push_back(std::move(tree));
	}

	// A mapping that matched no module is a typo, and a silent one: the module
	// it meant to redirect would keep using the variant's own name and quietly
	// contribute nothing.
	if (mapped != nullptr)
	{
		for (const auto& [name, layer] : *mapped)
		{
			const modules::ResolvedModule* module = m_modules.find(name);
			if (module == nullptr)
			{
				throw ncp::exception("Variant \"" + m_variant + "\" maps module \"" + name
					+ "\" to \"" + layer + "\", but no such module is enabled.");
			}
			if (module->def == nullptr || !module->def->nitrofs.declared)
			{
				throw ncp::exception("Variant \"" + m_variant + "\" maps module \"" + name
					+ "\" to \"" + layer + "\", but that module declares no "
					ANSI_bCYAN "nitrofs" ANSI_RESET " tree.");
			}
		}
	}

	// Memoized, because the same candidate comes up once per swept file below
	// it and answering means reading the container to see what it is. A build
	// touches one ROM, so the answers cannot go stale within it.
	std::map<std::string, bool> archives;
	const rom::ArchiveProbe isArchive = [&](std::string_view path) {
		const auto known = archives.find(std::string(path));
		if (known != archives.end())
			return known->second;

		bool answer = false;
		if (rom.findNitroFile(path) >= 0)
			answer = rom::narcWrapper(rom.readNitroFile(path)).has_value();
		archives.emplace(std::string(path), answer);
		return answer;
	};

	std::vector<config::FileConfig> files = trees.empty()
		? std::vector<config::FileConfig>()
		: rom::sweepFileTrees(trees, m_variant, isArchive);

	if (m_config.filesReserve.configured())
	{
		const std::string& reserved = m_config.filesReserve.value;
		const auto claimed = std::find_if(files.begin(), files.end(),
			[&](const config::FileConfig& file) { return file.path == reserved; });
		if (claimed != files.end())
		{
			throw ncp::exception("A NitroFS tree supplies \"" + reserved + "\", which "
				ANSI_bCYAN "files-reserve" ANSI_RESET " keeps empty."
				OREASONNL + claimed->source.string() + " would be given the file id the "
				"reservation exists to leave unused.");
		}
	}

	// An explicit `files:` entry is the project overruling the sweep, so it
	// replaces rather than collides.
	for (const config::FileConfig& file : m_config.files)
	{
		const auto existing = std::find_if(files.begin(), files.end(),
			[&](const config::FileConfig& swept) { return swept.path == file.path; });
		if (existing == files.end())
			files.push_back(file);
		else
			*existing = file;
	}

	return files;
}

rom::InsertionRecord Application::insertFiles(rom::RomAccessor& rom, bool planning)
{
	ScopedContext ctx(Diag::NitroFsInsert, planning
		? "Could not plan the NitroFS files."
		: "Could not insert the NitroFS files.");

	rom::InsertionRecord inserted;
	inserted.files = resolveNitroFiles(rom);
	const std::vector<config::FileConfig>& files = inserted.files;
	if (files.empty())
		return inserted;

	if (!planning)
		Log::info("Inserting NitroFS files...");

	struct PreparedFile
	{
		const config::FileConfig* config = nullptr;
		std::vector<u8> data;
		int existingId = -1;

		// '/'-separated path within a Nitro archive, empty for a loose file.
		std::string inner;

		// Planning only: the source was not on disk, so `data` is empty rather
		// than the file's contents.
		bool missing = false;
	};
	std::vector<PreparedFile> replacements;
	std::vector<PreparedFile> additions;
	std::vector<PreparedFile> claims;

	// Members to write into archives, grouped by the archive's own ROM path.
	// Grouped because one archive has to be opened, edited and written back
	// once however many of its members the build replaces, and ordered, so
	// that two runs of the same project produce the same bytes.
	std::map<std::string, std::vector<PreparedFile>> archives;

	// Read and validate everything before the first write. The directory
	// backend writes immediately, so discovering one bad source late would
	// otherwise leave the earlier files changed.
	for (const config::FileConfig& file : files)
	{
		const fs::path source = m_ctx.paths.work(file.source);
		const bool present = fs::exists(source) && fs::is_regular_file(source);

		// A build needs the bytes. A plan needs the destination, and a project
		// whose NitroFS sources are written by a pre-build hook has none of
		// them until it has been built once -- so refusing would make the
		// command useless exactly where it is most wanted. The destination is
		// planned with an empty file instead, and the entry says its source was
		// not there to be measured.
		if (!present)
		{
			if (!planning)
				throw ncp::file_error(source, ncp::file_error::find);
			inserted.missingSources.push_back(file.path);
		}

		PreparedFile prepared;
		prepared.config = &file;
		prepared.missing = !present;

		if (present)
		{
			const std::uintmax_t size = fs::file_size(source);
			if (size > std::numeric_limits<u32>::max())
				throw ncp::exception("NitroFS source is too large: " + source.string());

			prepared.data.resize(std::size_t(size));
			std::ifstream input(source, std::ios::binary);
			if (!input.is_open())
				throw ncp::file_error(source, ncp::file_error::read);
			if (!prepared.data.empty())
			{
				input.read(reinterpret_cast<char*>(prepared.data.data()), std::streamsize(prepared.data.size()));
				if (!input)
					throw ncp::file_error(source, ncp::file_error::read);
			}
		}

		const config::NitroDestination destination = config::splitNitroDestination(file.path);
		prepared.existingId = rom.findNitroFile(destination.path);

		if (destination.inArchive)
		{
			// The archive has to be there: this opens a container and replaces
			// one of its members, which is a different operation from creating
			// a file, and z_new/ is where new files go.
			if (prepared.existingId < 0)
			{
				throw ncp::exception("Cannot write \"" + destination.inner + "\" into \""
					+ destination.path + "\": the ROM has no such archive.");
			}
			if (file.id >= 0)
			{
				throw ncp::exception("Cannot claim a NitroFS file id for \"" + file.path
					+ "\": " ANSI_bCYAN "id" ANSI_RESET " renames a loose file, and this entry names "
					"a member of an archive.");
			}

			prepared.inner = destination.inner;
			archives[destination.path].push_back(std::move(prepared));
			continue;
		}

		if (file.id >= 0)
		{
			// Claiming an existing id. Everything the FNT will not catch is
			// checked here, because the failure mode of getting it wrong is a
			// quietly renamed neighbor rather than an error.
			const std::string current = rom.nitroFilePath(u32(file.id));
			if (current.empty())
			{
				throw ncp::exception("Cannot claim NitroFS file id " + std::to_string(file.id)
					+ " for \"" + file.path + "\": the ROM has no file with that id.");
			}
			if (prepared.existingId >= 0 && prepared.existingId != file.id)
			{
				throw ncp::exception("Cannot claim NitroFS file id " + std::to_string(file.id)
					+ " for \"" + file.path + "\": that path already exists as id "
					+ std::to_string(prepared.existingId) + "."
					OREASONNL "Drop the id to replace it in place.");
			}

			const auto shadowed = std::find_if(files.begin(), files.end(),
				[&](const config::FileConfig& other) {
					return &other != &file && other.id < 0 && other.path == current;
				});
			if (shadowed != files.end())
			{
				throw ncp::exception("Cannot claim NitroFS file id " + std::to_string(file.id)
					+ " for \"" + file.path + "\": \"" + current
					+ "\" is that id's current name and is also written in "
					ANSI_bCYAN "files" ANSI_RESET "."
					OREASONNL "One of the two would silently win depending on insertion order.");
			}

			prepared.existingId = file.id;
			claims.push_back(std::move(prepared));
		}
		else if (prepared.existingId >= 0)
		{
			replacements.push_back(std::move(prepared));
		}
		else if (file.path.starts_with("z_new/"))
		{
			additions.push_back(std::move(prepared));
		}
		else
		{
			std::ostringstream oss;
			oss << "Cannot replace NitroFS file " << OSTR(file.path)
			    << ": that path does not exist in the ROM." << OREASONNL
			    << "New files must be under z_new/.";

			// The archive-folder convention reads a segment as a container only
			// when the ROM holds one there, so a path that still has an
			// underscored segment in it is very often a misspelt archive rather
			// than a missing file. Say so: the alternative is the user
			// comparing two spellings by eye.
			for (std::size_t start = 0, slash = 0;
			     (slash = file.path.find('/', start)) != std::string::npos;
			     start = slash + 1)
			{
				const std::string segment = file.path.substr(start, slash - start);
				const std::size_t underscore = segment.rfind('_');
				if (underscore == std::string::npos || underscore == 0
					|| underscore + 1 >= segment.size())
					continue;

				oss << OREASONNL << OSTR(segment) << " reads as a directory because the ROM has "
				    << "no archive at " << OSTR(file.path.substr(0, start + underscore) + "."
					+ segment.substr(underscore + 1)) << ".";
				break;
			}

			throw ncp::exception(oss.str());
		}
	}

	// Archives, resolved before anything is written, like every other source
	// above, and for the same reason: the directory backend writes as it goes,
	// so a container that turns out not to be an archive has to be found before
	// its neighbor has already been repacked on disk.
	//
	// A plain replacement of the archive itself, if the build has one, is what
	// the members are applied on top of. That is the layered case working the
	// way it reads: a module supplies a whole container and another edits one
	// file inside it, and neither has to know about the other.
	struct RepackedArchive
	{
		std::string path;
		std::vector<u8> data;
		config::FileConfig summary;
		bool summarize = true;
	};
	std::vector<RepackedArchive> repacked;

	// What the manifest should say about an archive the build edited. The
	// entries that did the editing name members, which are not ROM files, so
	// the archive needs a record of its own, and it can only carry the
	// provenance its members agree on. One member gives the whole answer; five
	// from three modules give the honest one, which is that no single source
	// stands behind the file.
	auto archiveProvenance = [](const std::string& archivePath,
	                            const std::vector<PreparedFile>& edits) {
		config::FileConfig summary;
		summary.path = archivePath;
		if (edits.size() == 1)
			summary.source = edits.front().config->source;

		auto agreed = [&](std::string config::FileConfig::* field) {
			const std::string& first = edits.front().config->*field;
			for (const PreparedFile& edit : edits)
			{
				if (edit.config->*field != first)
					return std::string();
			}
			return first;
		};
		summary.module = agreed(&config::FileConfig::module);
		summary.component = agreed(&config::FileConfig::component);
		summary.fromVariant = agreed(&config::FileConfig::fromVariant);
		return summary;
	};

	for (const auto& [archivePath, edits] : archives)
	{
		RepackedArchive entry;
		entry.path = archivePath;

		// A wholesale replacement of the archive is folded in here rather than
		// written and then overwritten, and it is the entry the manifest should
		// credit: it supplied the file, the members only edited it.
		const auto wholesale = std::find_if(replacements.begin(), replacements.end(),
			[&](const PreparedFile& file) { return file.config->path == archivePath; });

		std::vector<u8> bytes;
		if (wholesale != replacements.end())
		{
			bytes = wholesale->data;
			entry.summarize = false;
		}
		else
		{
			bytes = rom.readNitroFile(archivePath);
			entry.summary = archiveProvenance(archivePath, edits);
		}

		// By content rather than by name: a container the ROM stores compressed
		// (Mario Kart DS names those `.carc`) is still an archive, and Narc
		// unwraps it and puts the wrapper back on the way out.
		if (!rom::narcWrapper(bytes))
		{
			std::ostringstream oss;
			oss << OSTRa(archivePath) << " is not a Nitro archive.";
			throw ncp::exception(oss.str());
		}

		rom::Narc narc = [&] {
			try
			{
				return rom::Narc::parse(bytes);
			}
			catch (const std::exception& e)
			{
				std::ostringstream oss;
				oss << "Could not read the Nitro archive " << OSTRa(archivePath) << "."
				    << OREASONNL << e.what();
				throw ncp::exception(oss.str());
			}
		}();

		for (const PreparedFile& edit : edits)
		{
			const int index = narc.findFile(edit.inner);
			if (index < 0)
			{
				// An error rather than a warning. A missing member means the
				// replacement silently did not happen, and the way that ships
				// is a ROM with the translation still in the wrong language.
				std::ostringstream oss;
				oss << OSTRa(archivePath) << " has no member " << OSTR(edit.inner) << "."
				    << OREASONNL << "It holds " << narc.fileCount() << " file(s)"
				    << (narc.allFiles().empty() ? ", none of them named." : ".");
				throw ncp::exception(oss.str());
			}
			narc.replaceFile(std::size_t(index), edit.data);

			// The manifest needs this and cannot recover it later: once the
			// container is written back, the ROM's table shows one modified
			// file and nothing at all about which of its members moved.
			rom::ArchiveEdit record;
			record.archive = archivePath;
			record.index = u32(index);
			record.member = edit.inner;
			record.size = u32(edit.data.size());
			record.source = edit.config->source;
			record.module = edit.config->module;
			record.component = edit.config->component;
			record.fromVariant = edit.config->fromVariant;
			record.sourceMissing = edit.missing;
			inserted.archiveEdits.push_back(std::move(record));
		}

		entry.data = narc.serialize();
		repacked.push_back(std::move(entry));

		if (wholesale != replacements.end())
			replacements.erase(wholesale);
	}

	// File ids inside one FNT directory are consecutive. Add a directory's own
	// files before any child directory, matching the established z_new layout.
	//
	// Ordered case-insensitively, with the raw path breaking ties so the order
	// is still total.
	//
	// This is a different question from how a path is looked up. Lookup has a
	// right answer: the FNT stores bytes, and NitroFs::findFile compares them
	// exactly so that a config naming one entry can never resolve to a
	// differently-cased neighbor. Ordering has no right answer: nothing reads
	// the order, it only has to be stable, so the rule to pick is the one a
	// person browsing an extracted ROM expects, which is the case-insensitive
	// one every mainstream desktop filesystem presents. It is also what already
	// shipped, since sorting bytewise instead puts SE_VOC_LU_SHOT ahead of
	// desyncwarn_top and renumbers five z_new files.
	std::sort(additions.begin(), additions.end(), [](const PreparedFile& left, const PreparedFile& right) {
		auto key = [](const std::string& path) {
			const std::size_t slash = path.rfind('/');
			const std::string directory = path.substr(0, slash);
			const std::string name = path.substr(slash + 1);
			auto folded = [](std::string text) {
				std::transform(text.begin(), text.end(), text.begin(),
					[](unsigned char c) { return char(std::tolower(c)); });
				return text;
			};
			return std::tuple(folded(directory), folded(name), directory, name);
		};
		return key(left.config->path) < key(right.config->path);
	});

	// The reserved placeholder, when the game needs one, goes in before any of
	// them: the whole point is to be given the first id an addition would
	// otherwise get. See ProjectConfig::filesReserve for why a game would.
	if (!additions.empty() && m_config.filesReserve.configured())
	{
		const std::string& reserved = m_config.filesReserve.value;
		if (rom.findNitroFile(reserved) < 0)
			inserted.createdIds.push_back(rom.addNitroFile(reserved, std::span<const u8>()));
	}

	auto report = [planning](const PreparedFile& file, u32 fileId, const char* action) {
		if (planning)
			return;
		Log::info(std::string(std::string_view(action) == "created" ? "Added " : "Replaced ")
			+ file.config->path + " [" + std::to_string(fileId) + "]");
		msg::Artifact artifact;
		artifact.kind = "file";
		artifact.action = action;
		artifact.name = file.config->path;
		artifact.size = static_cast<long long>(file.data.size());
		artifact.fileId = int(fileId);
		msg::artifact(std::move(artifact));
	};

	// Renames first: after this a claimed id answers to its new path, which is
	// what the ordinary replace below needs in order to find it.
	for (const PreparedFile& file : claims)
	{
		const u32 fileId = u32(file.existingId);
		if (rom.findNitroFile(file.config->path) != file.existingId)
			rom.renameNitroFile(fileId, file.config->path);
		(void)rom.replaceNitroFile(file.config->path, file.data);
		report(file, fileId, "modified");
	}
	for (const PreparedFile& file : replacements)
	{
		const u32 fileId = rom.replaceNitroFile(file.config->path, file.data);
		report(file, fileId, "modified");
	}

	for (const RepackedArchive& archive : repacked)
	{
		// The members first, then the file the ROM actually holds: that is the
		// order the work happened in, and it reads as one archive at a time
		// however many the build touched.
		for (const PreparedFile& edit : archives.at(archive.path))
		{
			if (planning)
				continue;
			Log::info("Replaced " + edit.config->path);
			msg::Artifact member;
			member.kind = "archive-file";
			member.action = "modified";
			member.name = edit.config->path;
			member.size = static_cast<long long>(edit.data.size());
			msg::artifact(std::move(member));
		}

		const u32 fileId = rom.replaceNitroFile(archive.path, archive.data);
		if (planning)
			continue;

		Log::info("Repacked " + archive.path + " [" + std::to_string(fileId) + "]");
		msg::Artifact artifact;
		artifact.kind = "file";
		artifact.action = "modified";
		artifact.name = archive.path;
		artifact.size = static_cast<long long>(archive.data.size());
		artifact.fileId = int(fileId);
		msg::artifact(std::move(artifact));
	}

	for (const PreparedFile& file : additions)
	{
		const u32 fileId = rom.addNitroFile(file.config->path, file.data);
		inserted.createdIds.push_back(fileId);
		report(file, fileId, "created");
	}

	// Last, because every PreparedFile above points into `inserted.files` and
	// growing it would move what those pointers name. The manifest reports ROM
	// files, and an edited archive is one: without a record of its own it would
	// come out as untouched, since the entries that changed it name members
	// rather than the file the ROM holds.
	for (RepackedArchive& archive : repacked)
	{
		if (archive.summarize)
			inserted.files.push_back(std::move(archive.summary));
	}

	return inserted;
}

void Application::insertBanner(rom::RomAccessor& rom) const
{
	fs::path configured = m_config.romBanner.value;

	// A variant may override it, though in practice one banner serves every
	// build: the region holds a title in all six console languages at once.
	const auto variant = std::find_if(m_config.variants.begin(), m_config.variants.end(),
		[&](const config::VariantConfig& candidate) { return candidate.name == m_variant; });
	if (variant != m_config.variants.end() && !variant->banner.empty())
		configured = variant->banner;

	if (configured.empty())
		return;

	ScopedContext ctx(Diag::NitroFsInsert, "Could not replace the ROM banner.");

	const fs::path source = m_ctx.paths.work(configured);
	if (!fs::exists(source) || !fs::is_regular_file(source))
		throw ncp::file_error(source, ncp::file_error::find);

	if (!rom.hasBanner())
	{
		std::ostringstream oss;
		oss << "This ROM has no icon/title banner to replace." << OREASONNL
		    << OSTR(rom.location().string()) << " does not have one.";
		throw ncp::exception(oss.str());
	}

	std::vector<u8> data(std::size_t(fs::file_size(source)));
	std::ifstream input(source, std::ios::binary);
	if (!input.is_open())
		throw ncp::file_error(source, ncp::file_error::read);
	if (!data.empty())
	{
		input.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size()));
		if (!input)
			throw ncp::file_error(source, ncp::file_error::read);
	}

	// The same refusal on both backends, so that a project which switches
	// between an extracted directory and a .nds does not discover the rule only
	// once it packs one. A banner's length is fixed by the version word it
	// starts with; a different length is a different banner format, not a
	// bigger one, and quietly relaying the container out around it would be the
	// wrong answer to a configuration mistake.
	const std::size_t current = rom.readBanner().size();
	if (data.size() != current)
	{
		std::ostringstream oss;
		oss << "Cannot replace the banner with " << OSTR(source.string()) << "." << OREASONNL
		    << "It is " << data.size() << " bytes and the ROM's banner is " << current << ".";
		throw ncp::exception(oss.str());
	}

	rom.writeBanner(data);

	Log::info("Replaced the ROM banner.");
	msg::Artifact artifact;
	artifact.kind = "banner";
	artifact.action = "modified";
	artifact.name = "banner";
	artifact.size = static_cast<long long>(data.size());
	msg::artifact(std::move(artifact));
}

void Application::writeFileDump(const rom::RomAccessor& rom, const rom::InsertionRecord& inserted) const
{
	if (!m_config.filesDump.configured())
		return;

	ScopedContext ctx(Diag::NitroFsInsert, "Could not write the file manifest.");

	const fs::path path = m_ctx.paths.work(m_config.filesDump.value);

	std::error_code error;
	fs::create_directories(path.parent_path(), error);

	std::ofstream file(path);
	if (!file.is_open())
	{
		std::ostringstream oss;
		oss << "Could not open " << OSTR(path.string()) << " for writing.";
		throw ncp::exception(oss.str());
	}

	const std::vector<rom::ManifestEntry> entries =
		rom::buildManifest(rom, inserted, m_ctx.paths.workDir);
	rom::writeManifest(file, entries, m_variant);
	Log::info("Wrote the file manifest.");
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

// v1 projects keep their targets in separate files, read later than the project
// file itself. They must see the same .ncpatcher.env the project file saw, or a
// pinned reference would apply to `includes` at the project level and not at the
// target level, which is where v1 projects actually put theirs.
config::V1Options Application::targetLoadOptions() const
{
	config::V1Options options;
	options.envFile = &m_envFile;
	return options;
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

	// Before the configuration, because ${env.NAME} in it may resolve here.
	if (!m_cli.noEnvFile)
	{
		m_envFile.load(m_ctx.paths.workDir / fs::path(config::EnvFile::DEFAULT_NAME));
		if (m_envFile.loaded() && !m_envFile.empty())
		{
			std::ostringstream oss;
			oss << "Read " << m_envFile.entries().size() << " variable(s) from "
			    << OSTRa(m_envFile.file().filename().string()) << ":";
			for (const auto& [name, value] : m_envFile.entries())
				oss << OREASONNL << name << "=" << value;
			Log::info(oss.str());
		}
	}

	m_config = config::load(projectFile(), m_ctx.paths.workDir, overrides, &m_envFile);

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
	// a configuration problem rather than an unanticipated one, and the exit
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
	// build get no log file unless one was asked for: `config dump` and
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
// whose common parent is code/, the source tree. A log is a build artifact,
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
