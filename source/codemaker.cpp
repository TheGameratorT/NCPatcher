#include "codemaker.hpp"

#include <unordered_map>
#include <fstream>
#include <thread>

#include "global.hpp"
#include "except.hpp"

// GCC 10 has a bug in Windows where the
// paths break during dependency generation,
// this macro works around that bug.
// If the bug gets fixed in newer versions,
// remove this macro.
#define GCC_HAS_DEP_PATH_BUG

namespace fs = std::filesystem;

static const char* ExtensionForSourceFileType[] = {
	".c", ".cpp", ".s"
};

static const char* ObjNameTypeForSourceFileType[] = {
	"C", "C++", "ASM"
};

static const char* CompilerForSourceFileType[] = {
	"gcc ", "g++ ", "gcc "
};

CodeMaker::CodeMaker(const BuildTarget& target) :
	target(target)
{
	setup();
	make();
}

void CodeMaker::setup()
{
	for (const BuildTarget::Region& region : target.regions)
	{
		BuiltRegion bregion;
		bregion.region = &region;
		std::vector<SourceFile> srcFiles;
		getSrcFiles(srcFiles, region, false);
		pushBuildFiles(srcFiles, region, bregion);
		builtRegions.push_back(bregion);
	}

	includeFlags += "-include\"" + (ncp::exe_dir / "ncp.h").string() + "\" ";
	for (const fs::path& include : target.includes)
		includeFlags += "-I\"" + include.string() + "\" ";
}

void CodeMaker::make()
{
	std::mutex mtx;
	std::vector<std::thread> threads;
	bool failed = false;

	for (auto& file : buildFiles)
	{
		fs::path objDestDir = file.obj.parent_path();
		if (!fs::exists(objDestDir))
		{
			if (!fs::create_directories(objDestDir))
			{
				std::ostringstream oss;
				oss << "Could not create object directory: " << OSTR(objDestDir);
				throw ncp::exception(oss.str());
			}
		}

		threads.emplace_back(buildSource, this, std::ref(failed), std::ref(mtx), std::ref(file));
	}

	for (std::thread& th : threads)
		th.join();

	if (failed)
		throw ncp::exception("Build failed.");
}

void CodeMaker::buildSource(bool& failed, std::mutex& mtx, const BuildSourceFile& file)
{
	std::ostringstream out;

	std::string srcS = file.src.string();
	std::string objS = file.obj.string();
	std::string depS = file.dep.string();

	out << OBUILD << "Building " << ObjNameTypeForSourceFileType[file.type] << " object " << OSTR(srcS) << " -> " << OSTR(objS) << std::endl;

	std::string ccmd = ncp::build_cfg.prefix;
	ccmd += CompilerForSourceFileType[file.type];
	ccmd += file.flags + " " + includeFlags;
	ccmd += "-c -fdiagnostics-color -MMD -MF \"" + depS + "\" ";
	ccmd += "\"" + srcS + "\" -o \"" + objS + "\"";

	int retcode = exec(ccmd.c_str(), out);
	out << std::flush;

	{
		std::lock_guard<std::mutex> lock(mtx);
		if (retcode != 0)
		{
			failed = true;
			out << "Error code: " << retcode << std::endl;
		}
		ansi::cout << out.str();
	}
}

void CodeMaker::getSrcFiles(std::vector<SourceFile>& srcFiles, const BuildTarget::Region& region, bool rebuildAll)
{
	for (auto& dir : region.sources)
	{
		for (auto& entry : fs::directory_iterator(dir))
		{
			if (entry.is_regular_file())
			{
				fs::path srcPath = entry.path();

				size_t fileType = util::index_of(srcPath.extension(), ExtensionForSourceFileType, 3);
				if (fileType == -1)
					continue;

				std::string buildPath = (target.build / srcPath).string();
				fs::path objPath = buildPath + ".o";
				fs::path depPath = buildPath + ".d";

				bool buildSrc;
				fs::file_time_type objTime;
				if (fs::exists(objPath) && !rebuildAll)
				{
					objTime = fs::last_write_time(objPath);
					buildSrc = false;
				}
				else
				{
					buildSrc = true;
				}

				SourceFile srcFile = {
					.src = srcPath,
					.obj = objPath,
					.dep = depPath,
					.objT = objTime,
					.type = fileType,
					.rebuild = buildSrc
				};
				srcFiles.push_back(srcFile);
			}
		}
	}
}

void CodeMaker::pushBuildFiles(std::vector<SourceFile>& srcFiles, const BuildTarget::Region& region, BuiltRegion& bregion)
{
	// Fetch dependencies to prevent multiple builds

	std::unordered_map<std::string, fs::file_time_type> timeForDep;

	for (SourceFile& srcFile : srcFiles)
	{
		if (srcFile.rebuild)
			continue;

		if (!fs::exists(srcFile.dep))
		{
			srcFile.rebuild = true;
			continue;
		}

		std::vector<fs::path> deps;

		std::ifstream depStrm(srcFile.dep);
		if (!depStrm.is_open())
		{
			srcFile.rebuild = true;
			continue;
		}

		std::string line;
		while (std::getline(depStrm, line))
		{
			if (line.ends_with(':'))
				continue;

			size_t pos1;
			size_t pos2 = line.length();

			if (!line.starts_with(' '))
				pos1 = line.find(':') + 2;
			else
				pos1 = 1;

			if (line.ends_with('\\'))
				pos2 -= 2;

			if (pos1 >= pos2)
				continue;

			std::string dep = line.substr(pos1, pos2 - pos1);

			std::string subLine;
			std::istringstream subStrm(dep);
			while (std::getline(subStrm, subLine, ' '))
			{
#ifdef GCC_HAS_DEP_PATH_BUG
				size_t pathBugPos = subLine.find("\\:");
				if (pathBugPos != std::string::npos)
					subLine.erase(subLine.begin() + pathBugPos);
#endif
				deps.push_back(subLine);
			}
		}

		depStrm.close();

		for (auto& dep : deps)
		{
			if (!fs::exists(dep))
			{
				srcFile.rebuild = true;
				continue;
			}

			std::string depS = dep.string();

			if (!timeForDep.contains(depS))
			{
				fs::file_time_type depT = fs::last_write_time(depS);
				timeForDep.insert(std::make_pair(depS, depT));
			}

			fs::file_time_type depT = timeForDep[depS];
			if (depT > srcFile.objT)
			{
				srcFile.rebuild = true;
				continue;
			}
		}
	}

	// Save the whole build to memory

	for (SourceFile& srcFile : srcFiles)
	{
		bregion.objects.push_back(srcFile.obj);

		if (!srcFile.rebuild)
			continue;

		std::string flags;
		switch (srcFile.type)
		{
		case SourceFileType::C:
			flags = region.cFlags;
			break;
		case SourceFileType::CPP:
			flags = region.cppFlags;
			break;
		case SourceFileType::ASM:
			flags = region.asmFlags;
			break;
		}

		BuildSourceFile bSrcFile;
		bSrcFile.src = srcFile.src;
		bSrcFile.obj = srcFile.obj;
		bSrcFile.dep = srcFile.dep;
		bSrcFile.type = srcFile.type;
		bSrcFile.flags = flags;
		buildFiles.push_back(bSrcFile);
	}
}

const std::vector<CodeMaker::BuiltRegion>& CodeMaker::getBuiltRegions() const
{
	return builtRegions;
}
