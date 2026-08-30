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
#include "../system/except.hpp"
#include "../system/log.hpp"
#include "../system/ansi.hpp"
#include "../system/message.hpp"
#include "../system/process.hpp"
#include "../system/paths.hpp"
#include "../system/diagnostics.hpp"
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
	core::CompilationUnitManager& compilationUnitMgr
	)
{
	m_target = &target;
	m_ctx = &ctx;
	m_paths = &ctx.paths;
	m_compilationUnitMgr = &compilationUnitMgr;

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
	for (const auto* unit : m_compilationUnitMgr->getUserUnits())
	{
		if (!unit->needsRebuild())
			continue;
		atLeastOneNeedsRebuild = true;
	}

	if (atLeastOneNeedsRebuild)
		compileSources();
	else
		Log::out << OBUILD << "Nothing needs building." << std::endl;
}

void ObjMaker::getSourceFiles()
{
	for (const BuildTarget::Region& region : m_target->regions)
	{
		for (auto& srcPath : region.sources)
		{
			std::size_t fileType = Util::indexOf(srcPath.extension(), ExtensionForSourceFileType, 3);
			if (fileType == -1)
				continue;

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
					
				safeBuildPath = m_paths->buildDir / "external" / pathStr;
			}
			else
			{
				// For relative paths, use the original behavior
				safeBuildPath = m_paths->buildDir / srcPath;
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

			core::CompilationUnit* unit = m_compilationUnitMgr->createCompilationUnit(
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
		}
	}
}

void ObjMaker::checkIfSourcesNeedRebuild()
{
	Log::info("Parsing object file dependencies...");

	// Fetch dependencies to prevent multiple builds
	std::unordered_map<std::string, fs::file_time_type> timeForDep;

	for (auto* unit : m_compilationUnitMgr->getUserUnits())
	{
		core::BuildInfo& buildInfo = unit->getBuildInfo();

		// Previously set as needing rebuild, no need to check.
		if (unit->needsRebuild())
			continue;

		// If the dependency file doesn't exist,
		// then we can't be sure if the object is up-to-date.
		if (!fs::exists(buildInfo.dependencyPath))
		{
			unit->setNeedsRebuild(true);
			continue;
		}

		std::vector<fs::path> deps;

		// If the dependency file can't be open,
		// then we can't be sure if the object is up-to-date.
		std::ifstream depStrm(buildInfo.dependencyPath);
		if (!depStrm.is_open())
		{
			unit->setNeedsRebuild(true);
			continue;
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
}

void ObjMaker::compileSources()
{
	BS::thread_pool pool(m_ctx->threadCount());

	BuildLogger logger;
	logger.setUnits(m_compilationUnitMgr->getUserUnits());
	logger.start(m_paths->targetWorkDir);

	// Counted up front rather than as they are pushed, because a progress event
	// is only worth anything to the caller if it knows the denominator.
	std::size_t pendingCount = 0;
	for (const auto* unit : m_compilationUnitMgr->getUserUnits())
		pendingCount += unit->needsRebuild() ? 1 : 0;

	auto completed = std::make_shared<std::atomic<std::size_t>>(0);

	std::size_t jobID = 0;
	for (auto* unit : m_compilationUnitMgr->getUserUnits())
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

		buildInfo.jobId = jobID++;
		buildInfo.buildStarted = false;
		buildInfo.logFinished = false;
		buildInfo.buildComplete = false;
		buildInfo.buildFailed = false;
		buildInfo.compilerDiagnosticsParsed = ncp::msg::isJson();

		pool.push_task([unit, this, completed, pendingCount](){
			core::BuildInfo& buildInfo = unit->getBuildInfo();
			
			buildInfo.buildStarted = true;

			std::ostringstream out;
			auto runCompiler = [&](const std::string& command) {
				std::ostringstream commandOut;
				const int retcode = Process::start(command.c_str(), m_paths->targetWorkDir, &commandOut);
				if (!ncp::msg::isJson())
				{
					out << commandOut.str();
					return retcode;
				}

				const auto diagnostics = ncp::build::parseGccDiagnostics(
					commandOut.str(), unit->getSourcePath().string());
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
				ccmd += ncp::msg::isJson()
					? "-c -fdiagnostics-format=json -fdiagnostics-color=never -fdata-sections -ffunction-sections "
					: "-c -fdiagnostics-color -fdata-sections -ffunction-sections ";
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

	auto timeStart = std::chrono::high_resolution_clock::now();
	while (pool.get_tasks_total() != 0)
	{
		auto timeNow = std::chrono::high_resolution_clock::now();
		if (timeNow >= timeStart + 250ms)
		{
			logger.update();
			timeStart = timeNow;
		}
	}

	pool.wait_for_tasks();

	logger.finish();

	if (logger.getFailed())
	{
		// The human already has the compiler's own output from BuildLogger.
		// This is the same information addressed to a tool: which files failed,
		// and what the compiler said about each.
		for (const auto* unit : m_compilationUnitMgr->getUserUnits())
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
