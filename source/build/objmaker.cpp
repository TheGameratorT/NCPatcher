#include "objmaker.hpp"

#include <atomic>
#include <fstream>
#include <thread>
#include <chrono>
#include <unordered_map>
#include <sstream>
#include <algorithm>

#include <BS_thread_pool.hpp>

#include "../config/buildtarget.hpp"
#include "../system/cancel.hpp"
#include "../system/except.hpp"
#include "../system/log.hpp"
#include "../system/ansi.hpp"
#include "../system/message.hpp"
#include "../system/process.hpp"
#include "../system/paths.hpp"
#include "../system/diagnostics.hpp"
#include "../system/cache.hpp"
#include "../formats/elf.hpp"
#include "../core/compilation_unit_manager.hpp"
#include "../utils/base32.hpp"
#include "../utils/util.hpp"
#include "../utils/unicode.hpp"
#include "buildlogger.hpp"
#include "gcc_diagnostics.hpp"

#include <functional>

// GCC 10 has a bug in Windows where the
// paths break during dependency generation,
// this macro works around that bug.
// If the bug gets fixed in newer versions,
// remove this macro.
#ifndef GCC_HAS_DEP_PATH_BUG
#define GCC_HAS_DEP_PATH_BUG 0
#endif

namespace fs = std::filesystem;

using namespace std::chrono_literals;

static const char* ExtensionForSourceFileType[] = { ".c", ".cpp", ".s" };
static const char* CompilerForSourceFileType[] = { "gcc ", "g++ ", "gcc " };
static const char* DefineForSourceFileType[] = { "__ncp_lang_c", "__ncp_lang_cpp", "__ncp_lang_asm" };

struct SourceFileType {
	enum {
		C = 0, CPP = 1, ASM = 2
	};
};

ObjMaker::ObjMaker() = default;

void ObjMaker::makeTarget(
	const BuildTarget& target,
	const ncp::Context& ctx,
	core::CompilationUnitManager& compilationUnitMgr,
	std::vector<std::unique_ptr<core::CompilationUnitManager>>& runtimeUnitMgrs
	)
{
	m_target = &target;
	m_ctx = &ctx;
	m_paths = &ctx.paths;
	m_compilationUnitMgr = &compilationUnitMgr;
	m_runtimeUnitMgrs = &runtimeUnitMgrs;

	// Not m_paths->appDir any more: the header is a data file with an install
	// location of its own, and it is checked for version agreement before a
	// single translation unit is compiled against it.
	fs::path ncpInclude;
	{
		ncp::ScopedContext headerContext(ncp::Diag::SdkHeaderMissing,
			"The NCPatcher SDK header is not usable.");
		ncpInclude = ncp::paths::sdkHeader();
	}

	m_includeFlags.reserve(256);
	m_includeFlags += "-include\"" + ncp::pathToUtf8(ncpInclude) + "\" ";
	for (const fs::path& include : m_target->includes)
		m_includeFlags += "-I\"" + ncp::pathToUtf8(include) + "\" ";

	// Build define flags from command line arguments
	m_defineFlags.clear();
	const std::vector<std::string>& defines = m_ctx->defines();
	for (const std::string& define : defines) {
		m_defineFlags += "-D";
		m_defineFlags += define;
		m_defineFlags += " ";
	}

	getSourceFiles();
	checkIfSourcesNeedRebuild();

	bool atLeastOneNeedsRebuild = false;
	for (const auto* unit : allUserUnits())
	{
		if (!unit->needsRebuild())
			continue;
		atLeastOneNeedsRebuild = true;
	}

	if (atLeastOneNeedsRebuild)
		compileSources();

	// Everything queued so far is built, so a second compile below only
	// takes the runtime.
	for (auto* unit : allUserUnits())
		unit->setNeedsRebuild(false);

	if (addRuntimeIfUsed())
	{
		atLeastOneNeedsRebuild = true;
		compileSources();
	}

	if (!atLeastOneNeedsRebuild)
		Log::step("Compiling", "up to date");
}

void ObjMaker::getSourceFiles()
{
	auto addRegion = [&](const BuildTarget::Region& region, core::CompilationUnitManager& unitMgr, const fs::path& buildRoot) {
		for (auto& srcPath : region.sources)
		{
			std::size_t fileType = Util::indexOf(srcPath.extension(), ExtensionForSourceFileType, 3);
			if (fileType == -1)
				continue;
			addSourceUnit(srcPath, region, fileType, unitMgr, buildRoot);
		}
	};

	for (const BuildTarget::Region& region : m_target->regions)
		addRegion(region, *m_compilationUnitMgr, m_paths->buildDir);

	// A module's objects get a directory of their own, so a source it shares
	// with the project or with another module is compiled under its flags.
	for (std::size_t i = 0; i < m_target->runtimeRegions.size(); i++)
	{
		const BuildTarget::Region& region = m_target->runtimeRegions[i];
		addRegion(region, *(*m_runtimeUnitMgrs)[i], m_paths->buildDir / "rt" / region.runtimeName);
	}
}

std::vector<core::CompilationUnit*> ObjMaker::allUserUnits() const
{
	std::vector<core::CompilationUnit*> units = m_compilationUnitMgr->getUserUnits();
	for (const auto& unitMgr : *m_runtimeUnitMgrs)
		units.insert(units.end(), unitMgr->getUserUnits().begin(), unitMgr->getUserUnits().end());
	return units;
}

core::CompilationUnit* ObjMaker::addSourceUnit(const fs::path& srcPath, const BuildTarget::Region& region, std::size_t fileType,
	core::CompilationUnitManager& unitMgr, const fs::path& buildRoot)
{
	// Create a safe build path that works for sources both inside and outside the project
	fs::path safeBuildPath;
	if (srcPath.is_absolute())
	{
		// For absolute paths, create a relative path structure under build directory
		// Convert absolute path to a safe relative structure by replacing path separators
		std::string pathStr = srcPath.string();
		
		// Replace drive letter colon and path separators with underscores for safety
		std::replace(pathStr.begin(), pathStr.end(), ':', '_');
		std::replace(pathStr.begin(), pathStr.end(), '\\', '_');
		std::replace(pathStr.begin(), pathStr.end(), '/', '_');
		
		// Remove any leading path separator replacements
		if (pathStr.front() == '_')
			pathStr = pathStr.substr(1);
			
		safeBuildPath = buildRoot / "external" / pathStr;
	}
	else
	{
		// For relative paths, use the original behavior
		safeBuildPath = buildRoot / srcPath;
	}
	
	std::string buildPath = safeBuildPath.string();
	fs::path objPath = buildPath + ".o";
	fs::path depPath = buildPath + ".d";
	fs::path asmPath = buildPath + ".s";

	bool buildSrc;
	fs::file_time_type objTime;
	if (fs::exists(objPath) && !m_target->getForceRebuild())
	{
		objTime = fs::last_write_time(objPath);
		buildSrc = false;
	}
	else
	{
		buildSrc = true;
	}

	core::CompilationUnit* unit = unitMgr.createCompilationUnit(
		core::CompilationUnitType::UserSourceFile,
		srcPath,
		objPath
	);
	
	unit->setTargetRegion(&region);
	unit->setNeedsRebuild(buildSrc);
	
	// Set up build info
	core::BuildInfo& buildInfo = unit->getBuildInfo();
	buildInfo.dependencyPath = depPath;
	buildInfo.assemblyPath = asmPath;
	buildInfo.objectWriteTime = objTime;
	buildInfo.fileType = fileType;

	return unit;
}

// ncprt.c is compiled into the main region only when an object calls into
// it, so projects that never apply a patch group at runtime do not carry it.
// A project that compiles its own copy keeps it.
bool ObjMaker::addRuntimeIfUsed()
{
	// The functions ncprt.c defines.
	static constexpr std::string_view RuntimeSymbols[] = {
		"__ncprt_apply", "__ncprt_revert", "__ncprt_encode", "ncprt_sync_code"
	};

	// A module that calls the runtime links against the main binary's copy,
	// so only the project's own regions can provide it.
	bool referenced = false;
	bool defined = false;
	for (auto* unit : allUserUnits())
	{
		const bool inModule = !unit->getTargetRegion()->runtimeName.empty();
		Elf32* elf = ncp::cache::CacheManager::getInstance().getOrLoadElf(unit->getObjectPath());
		Elf32::forEachSymbol(*elf, elf->getHeader(), elf->getSectionHeaderTable(),
		[&](const Elf32_Sym& symbol, std::string_view symbolName) -> bool {
			if (std::find(std::begin(RuntimeSymbols), std::end(RuntimeSymbols), symbolName) == std::end(RuntimeSymbols))
				return false;
			if (symbol.st_shndx == SHN_UNDEF)
				referenced = true;
			else if (ELF32_ST_BIND(symbol.st_info) == STB_GLOBAL && !inModule)
				defined = true;
			return false;
		});
	}

	if (!referenced || defined)
		return false;

	const BuildTarget::Region* mainRegion = m_target->getMainRegion();
	if (mainRegion == nullptr)
	{
		std::ostringstream oss;
		oss << "The runtime is used, but there is no main region to compile " << OSTR("ncprt.c") << " into.";
		throw ncp::exception(oss.str());
	}

	fs::path runtimeSource = ncp::paths::findDataFile("ncprt.c");
	if (runtimeSource.empty())
	{
		std::ostringstream oss;
		oss << "Could not find " << OSTR("ncprt.c") << ", which the runtime is compiled from. Searched:\n"
		    << ncp::paths::dataDirList();
		throw ncp::exception(oss.str());
	}

	core::CompilationUnit* unit = addSourceUnit(runtimeSource, *mainRegion, SourceFileType::C, *m_compilationUnitMgr, m_paths->buildDir);
	if (!unit->needsRebuild())
	{
		std::unordered_map<std::string, fs::file_time_type> timeForDep;
		checkIfUnitNeedsRebuild(unit, timeForDep);
	}
	return unit->needsRebuild();
}

void ObjMaker::checkIfSourcesNeedRebuild()
{
	// Fetch dependencies to prevent multiple builds
	std::unordered_map<std::string, fs::file_time_type> timeForDep;

	for (auto* unit : allUserUnits())
	{
		// Previously set as needing rebuild, no need to check.
		if (!unit->needsRebuild())
			checkIfUnitNeedsRebuild(unit, timeForDep);
	}
}

void ObjMaker::checkIfUnitNeedsRebuild(core::CompilationUnit* unit, std::unordered_map<std::string, fs::file_time_type>& timeForDep)
{
	core::BuildInfo& buildInfo = unit->getBuildInfo();

	// If the dependency file doesn't exist,
	// then we can't be sure if the object is up-to-date.
	if (!fs::exists(buildInfo.dependencyPath))
	{
		unit->setNeedsRebuild(true);
		return;
	}

	std::vector<fs::path> deps;

	// If the dependency file can't be open,
	// then we can't be sure if the object is up-to-date.
	std::ifstream depStrm(buildInfo.dependencyPath);
	if (!depStrm.is_open())
	{
		unit->setNeedsRebuild(true);
		return;
	}

	std::string line;
	while (std::getline(depStrm, line))
	{
		std::string_view trimLine;
		trimLine = line.ends_with('\\') ?
			std::string_view(line).substr(0, line.find_last_of(' ', line.size() - 1)) :
			line;

		if (trimLine.starts_with(' '))
			trimLine = trimLine.substr(1);

		std::string trimLineStr(trimLine);
		std::string subLine;
		std::istringstream subStrm(trimLineStr);
		while (std::getline(subStrm, subLine, ' '))
		{
			if (subLine.ends_with(':'))
				continue;
#ifdef GCC_HAS_DEP_PATH_BUG
			std::size_t pathBugPos = subLine.find("\\:");
			if (pathBugPos != std::string::npos)
				subLine.erase(subLine.begin() + pathBugPos);
#endif
			deps.emplace_back(subLine);
		}
	}

	depStrm.close();

	for (auto& dep : deps)
	{
		// gcc writes these relative to the directory it was run in, which is
		// the target work dir, not wherever ncpatcher happens to sit. Anchor
		// them, or every dependency reads as missing and nothing is ever
		// considered up to date.
		dep = m_paths->target(dep);

		if (!fs::exists(dep))
		{
			unit->setNeedsRebuild(true);
			continue;
		}

		std::string depS = dep.string();

		if (!timeForDep.contains(depS))
		{
			fs::file_time_type depT = fs::last_write_time(dep);
			timeForDep.insert(std::make_pair(depS, depT));
		}

		fs::file_time_type depFileWriteTime = timeForDep[depS];
		if (depFileWriteTime > buildInfo.objectWriteTime)
		{
			unit->setNeedsRebuild(true);
			continue;
		}
	}
}

void ObjMaker::compileSources()
{
	BS::thread_pool pool(m_ctx->threadCount());

	// Once, before anything is queued: every unit uses the same answer, and
	// asking from inside the pool would have every thread ask at once.
	m_diagnosticsFormat = ncp::msg::isJson()
		? ncp::build::detectDiagnosticsFormat(m_ctx->toolchain() + "gcc", m_paths->buildDir)
		: ncp::build::DiagnosticsFormat::Text;

	const std::vector<core::CompilationUnit*> units = allUserUnits();

	BuildLogger logger;
	logger.setUnits(units);
	logger.start();

	// Counted up front rather than as they are pushed, because a progress event
	// is only worth anything to the caller if it knows the denominator.
	std::size_t pendingCount = 0;
	for (const auto* unit : units)
		pendingCount += unit->needsRebuild() ? 1 : 0;

	auto completed = std::make_shared<std::atomic<std::size_t>>(0);

	for (auto* unit : units)
	{
		if (!unit->needsRebuild())
			continue;

		fs::path objDestDir = unit->getObjectPath().parent_path();
		if (!fs::exists(objDestDir))
		{
			if (!fs::create_directories(objDestDir))
			{
				std::ostringstream oss;
				oss << "Could not create object directory: " << OSTR(objDestDir);
				throw ncp::exception(oss.str());
			}
		}

		core::BuildInfo& buildInfo = unit->getBuildInfo();

		buildInfo.buildStarted = false;
		buildInfo.buildComplete = false;
		buildInfo.buildFailed = false;
		buildInfo.compilerDiagnosticsParsed =
			m_diagnosticsFormat != ncp::build::DiagnosticsFormat::Text;

		pool.push_task([unit, this, completed, pendingCount](){
			core::BuildInfo& buildInfo = unit->getBuildInfo();

			// A queued compilation that has not started yet is simply dropped,
			// so a cancelled build drains rather than compiling the other two
			// hundred files on the way out. One already running finishes; its
			// object is complete and correct, and the next build will not have
			// to redo it.
			if (ncp::cancel::requested())
			{
				buildInfo.buildComplete = true;
				return;
			}

			buildInfo.startTime = std::chrono::steady_clock::now();
			buildInfo.buildStarted.store(true, std::memory_order_release);

			std::ostringstream out;
			auto runCompiler = [&](const std::string& command) {
				std::ostringstream commandOut;
				const int retcode = Process::start(command.c_str(), m_paths->targetWorkDir, &commandOut);
				if (m_diagnosticsFormat == ncp::build::DiagnosticsFormat::Text)
				{
					out << commandOut.str();
					return retcode;
				}

				const auto diagnostics = ncp::build::parseGccDiagnostics(
					commandOut.str(), unit->getSourcePath().string(), m_diagnosticsFormat);
				if (!diagnostics.has_value())
				{
					buildInfo.compilerDiagnosticsParsed = false;
					out << commandOut.str();
					return retcode;
				}

				for (const ncp::build::GccDiagnostic& diagnostic : *diagnostics)
					ncp::msg::diagnostic(diagnostic.level, ncp::Diag::TargetCompile,
						diagnostic.message, diagnostic.location);
				out << ncp::build::formatGccDiagnostics(*diagnostics);
				return retcode;
			};

			std::string srcS = ncp::pathToUtf8(unit->getSourcePath());
			std::string objS = ncp::pathToUtf8(unit->getObjectPath());
			std::string depS = ncp::pathToUtf8(buildInfo.dependencyPath);

			const BuildTarget::Region* region = unit->getTargetRegion();

			auto makeBuildCmd = [&](
				bool outputDeps, std::size_t fileType,
				const std::string& inputFile, const std::string& outputFile)
			{
				const std::string& flags = [&](){
					switch (fileType)
					{
					case SourceFileType::C:
						return region->cFlags;
					case SourceFileType::CPP:
						return region->cppFlags;
					case SourceFileType::ASM:
						return region->asmFlags;
					default:
						throw ncp::exception("Tried to get flags of invalid file type.");
					}
				}();

				std::string ccmd;
				ccmd.reserve(256);
				ccmd += m_ctx->toolchain();
				ccmd += CompilerForSourceFileType[fileType];
				ccmd += flags;
				if (fileType != SourceFileType::ASM)
					ccmd += " -S";
				ccmd += " -D";
				ccmd += DefineForSourceFileType[fileType];
				ccmd += " ";
				if (fileType != SourceFileType::ASM)
				{
					// base32 is symver compatible
					std::string __ncp_src_base32 = base32::encode_nopad(srcS);
					ccmd += " -D__ncp_src_base32=";
					ccmd += __ncp_src_base32;
					ccmd += " ";
				}
				ccmd += m_defineFlags;
				ccmd += m_includeFlags;
				if (m_diagnosticsFormat != ncp::build::DiagnosticsFormat::Text)
				{
					ccmd += "-c";
					ccmd += ncp::build::diagnosticsFormatFlag(m_diagnosticsFormat);
					ccmd += " -fdiagnostics-color=never -fdata-sections -ffunction-sections ";
				}
				else
				{
					ccmd += "-c -fdiagnostics-color -fdata-sections -ffunction-sections ";
				}
				if (outputDeps)
				{
					ccmd += "-MMD -MF \"";
					ccmd += depS;
					ccmd += "\" ";
				}
				ccmd += "\"";
				ccmd += inputFile;
				ccmd += "\" -o \"";
				ccmd += outputFile;
				ccmd += "\"";
				return ccmd;
			};

			// One place to report a unit as done, so the progress count cannot
			// drift from the units that actually finished.
			auto reportProgress = [&]() {
				ncp::msg::progress("compile", completed->fetch_add(1) + 1, pendingCount,
					unit->getSourcePath().string());
			};

			if (buildInfo.fileType != SourceFileType::ASM)
			{
				std::string asmS = ncp::pathToUtf8(buildInfo.assemblyPath);

				std::string ccmd = makeBuildCmd(true, buildInfo.fileType, srcS, asmS);

				// Several of these run at once, under the live block, so a
				// direct write to Log::out here would tear its display apart
				// (and race on Log's shared mode). The command line goes into
				// this unit's own buffer instead, the same one the compiler's
				// diagnostics do, and reaches the console with them once the
				// live block has settled.
				if (m_ctx->isVerbose(ncp::VerboseTag::Build))
					out << ccmd << "\n";

				int retcode = runCompiler(ccmd);
				if (retcode != 0)
				{
					buildInfo.buildFailed = true;
					out << "Exit code: " << retcode << "\n";
					buildInfo.buildOutput = out.str();
					buildInfo.buildComplete = true;
					reportProgress();
					return;
				}

				srcS = asmS;
			}

			std::string ccmd = makeBuildCmd(buildInfo.fileType == SourceFileType::ASM, SourceFileType::ASM, srcS, objS);

			if (m_ctx->isVerbose(ncp::VerboseTag::Build))
				out << ccmd << "\n";

			int retcode = runCompiler(ccmd);
			if (retcode != 0)
			{
				buildInfo.buildFailed = true;
				out << "Exit code: " << retcode << "\n";
			}
			buildInfo.buildOutput = out.str();
			buildInfo.buildComplete = true;
			reportProgress();
		});
	}

	while (pool.get_tasks_total() != 0)
	{
		std::this_thread::sleep_for(100ms);
		logger.update();
	}

	pool.wait_for_tasks();

	logger.finish();

	// Before the failure check: a cancelled build's dropped compilations are
	// not compile errors, and reporting them as such would send a caller
	// looking for a mistake in their code.
	ncp::cancel::checkpoint();

	if (logger.getFailed())
	{
		// The human already has the compiler's own output from BuildLogger.
		// This is the same information addressed to a tool: which files failed,
		// and what the compiler said about each.
		for (const auto* unit : units)
		{
			const core::BuildInfo& buildInfo = unit->getBuildInfo();
			if (!buildInfo.buildFailed)
				continue;
			if (buildInfo.compilerDiagnosticsParsed)
				continue;

			ncp::msg::Location location;
			location.file = unit->getSourcePath().string();
			ncp::msg::diagnostic(ncp::msg::Level::Error, ncp::Diag::TargetCompile,
				Ansi::strip(buildInfo.buildOutput), location);
		}

		throw ncp::exception("Compilation failed.");
	}
}
