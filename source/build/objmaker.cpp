#include "objmaker.hpp"

#include <fstream>
#include <thread>
#include <chrono>
#include <unordered_map>
#include <sstream>

#include <BS_thread_pool.hpp>

#include "../main.hpp"
#include "../util.hpp"
#include "../config/buildconfig.hpp"
#include "../except.hpp"
#include "../log.hpp"
#include "../process.hpp"
#include "buildlogger.hpp"

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
	const fs::path& targetWorkDir,
	const fs::path& buildDir,
	std::vector<std::unique_ptr<SourceFileJob>>& jobs
	)
{
	m_target = &target;
	m_targetWorkDir = &targetWorkDir;
	m_buildDir = &buildDir;
	m_jobs = &jobs;

	fs::path curPath = fs::current_path();

	fs::current_path(*m_targetWorkDir);

	fs::path ncpInclude = Main::getAppPath() / "ncp.h";
	if (!fs::exists(ncpInclude))
		throw ncp::file_error(ncpInclude, ncp::file_error::find);

	m_includeFlags.reserve(256);
	m_includeFlags += "-include\"" + ncpInclude.string() + "\" ";
	for (const fs::path& include : m_target->includes)
		m_includeFlags += "-I\"" + include.string() + "\" ";

	getSourceFiles();
	checkIfSourcesNeedRebuild();

	bool atLeastOneNeedsRebuild = false;
	for (std::unique_ptr<SourceFileJob>& srcFile : *m_jobs)
	{
		if (!srcFile->rebuild)
			continue;
		atLeastOneNeedsRebuild = true;
	}

	if (atLeastOneNeedsRebuild)
		compileSources();
	else
		Log::out << OBUILD << "Nothing needs building." << std::endl;

	fs::current_path(curPath);
}

void ObjMaker::getSourceFiles()
{
	for (const BuildTarget::Region& region : m_target->regions)
	{
		for (auto& dir : region.sources)
		{
			for (auto& entry : fs::directory_iterator(dir))
			{
				if (entry.is_regular_file())
				{
					const fs::path& srcPath = entry.path();

					std::size_t fileType = Util::indexOf(srcPath.extension(), ExtensionForSourceFileType, 3);
					if (fileType == -1)
						continue;

					std::string buildPath = (*m_buildDir / srcPath).string();
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

					auto srcFile = std::make_unique<SourceFileJob>();
					srcFile->srcFilePath = srcPath;
					srcFile->objFilePath = objPath;
					srcFile->depFilePath = depPath;
					srcFile->asmFilePath = asmPath;
					srcFile->objFileWriteTime = objTime;
					srcFile->fileType = fileType;
					srcFile->region = &region;
					srcFile->rebuild = buildSrc;
					m_jobs->emplace_back(std::move(srcFile));
				}
			}
		}
	}
}

void ObjMaker::checkIfSourcesNeedRebuild()
{
	Log::info("Parsing object file dependencies...");

	// Fetch dependencies to prevent multiple builds

	std::unordered_map<std::string, fs::file_time_type> timeForDep;

	for (std::unique_ptr<SourceFileJob>& srcFile : *m_jobs)
	{
		// Previously set as needing rebuild, no need to check.
		if (srcFile->rebuild)
			continue;

		// If the dependency file doesn't exist,
		// then we can't be sure if the object is up-to-date.
		if (!fs::exists(srcFile->depFilePath))
		{
			srcFile->rebuild = true;
			continue;
		}

		std::vector<fs::path> deps;

		// If the dependency file can't be open,
		// then we can't be sure if the object is up-to-date.
		std::ifstream depStrm(srcFile->depFilePath);
		if (!depStrm.is_open())
		{
			srcFile->rebuild = true;
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
			if (!fs::exists(dep))
			{
				srcFile->rebuild = true;
				continue;
			}

			std::string depS = dep.string();

			if (!timeForDep.contains(depS))
			{
				fs::file_time_type depT = fs::last_write_time(dep);
				timeForDep.insert(std::make_pair(depS, depT));
			}

			fs::file_time_type depFileWriteTime = timeForDep[depS];
			if (depFileWriteTime > srcFile->objFileWriteTime)
			{
				srcFile->rebuild = true;
				continue;
			}
		}
	}
}

void ObjMaker::compileSources()
{
	BS::thread_pool pool(BuildConfig::getThreadCount());

	BuildLogger logger;
	logger.setJobs(*m_jobs);
	logger.start(*m_targetWorkDir);

	std::size_t jobID = 0;
	for (std::unique_ptr<SourceFileJob>& srcFile : *m_jobs)
	{
		if (!srcFile->rebuild)
			continue;

		fs::path objDestDir = srcFile->objFilePath.parent_path();
		if (!fs::exists(objDestDir))
		{
			if (!fs::create_directories(objDestDir))
			{
				std::ostringstream oss;
				oss << "Could not create object directory: " << OSTR(objDestDir);
				throw ncp::exception(oss.str());
			}
		}

		srcFile->jobID = jobID++;
		srcFile->buildStarted = false;
		srcFile->logWasFinished = false;
		srcFile->finished = false;
		srcFile->failed = false;

		pool.push_task([&](){
			srcFile->buildStarted = true;

			std::ostringstream out;

			std::string srcS = srcFile->srcFilePath.string();
			std::string objS = srcFile->objFilePath.string();
			std::string depS = srcFile->depFilePath.string();

			const BuildTarget::Region* region = srcFile->region;

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
				ccmd += BuildConfig::getToolchain();
				ccmd += CompilerForSourceFileType[fileType];
				ccmd += flags;
				if (fileType != SourceFileType::ASM)
					ccmd += " -S";
				ccmd += " -D";
				ccmd += DefineForSourceFileType[fileType];
				ccmd += " ";
				ccmd += m_includeFlags;
				ccmd += "-c -fdiagnostics-color -fdata-sections -ffunction-sections ";
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

			if (srcFile->fileType != SourceFileType::ASM)
			{
				std::string asmS = srcFile->asmFilePath.string();

				std::string ccmd = makeBuildCmd(true, srcFile->fileType, srcS, asmS);

				int retcode = Process::start(ccmd.c_str(), &out);
				if (retcode != 0)
				{
					srcFile->failed = true;
					out << "Exit code: " << retcode << "\n";
					srcFile->output = out.str();
					srcFile->finished = true;
					return;
				}

				srcS = asmS;
			}

			std::string ccmd = makeBuildCmd(srcFile->fileType == SourceFileType::ASM, SourceFileType::ASM, srcS, objS);

			int retcode = Process::start(ccmd.c_str(), &out);
			if (retcode != 0)
			{
				srcFile->failed = true;
				out << "Exit code: " << retcode << "\n";
			}
			srcFile->output = out.str();
			srcFile->finished = true;
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
		throw ncp::exception("Compilation failed.");
}
