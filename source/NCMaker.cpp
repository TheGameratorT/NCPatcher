#include "NCMaker.hpp"

#include <iostream>
#include <fstream>
#include <sstream>
#include <thread>
#include <unordered_map>

#include "NCException.hpp"
#include "Process.hpp"
#include "Common.hpp"

// GCC 10 has a bug in Windows where the
// paths break during dependency generation,
// this macro works around that bug.
// If the bug gets fixed in newer versions,
// remove this macro.
#define GCC_HAS_DEP_PATH_BUG

namespace fs = std::filesystem;

static const char* CompileErr = "Compilation failed.";

static const char* build_cache = "build_cache.bin";
static const char* build_config = "build_config.json";

static const char* ExtensionForSourceFileType[] = {
	".c", ".cpp", ".s"
};

static const char* ObjNameTypeForSourceFileType[] = {
	"C", "C++", "ASM"
};

static const char* CompilerForSourceFileType[] = {
	"gcc ", "g++ ", "gcc "
};

NCMaker::NCMaker(const fs::path& workDir, const ConfigManager& cfgMgr, u32 codeAddr)
{
	this->workDir = workDir;
	this->cfgMgr = &cfgMgr;
	this->codeAddr = codeAddr;
}

void NCMaker::runProcess(const std::string& cmd, std::ostream* out)
{
	Process proc;
	proc.setOutput(out);
	proc.setApp(cmd);
	if (!proc.start())
	{
		throw NC::exception(CompileErr, "Could not start process.");
	}

	int errCode = proc.getExitCode();
	if (errCode != 0)
	{
		std::ostringstream oss;
		oss << "Error code: " << errCode;
		throw NC::exception(CompileErr, oss.str());
	}
}

void NCMaker::getSrcFiles(std::vector<SourceFile>& srcFiles, const std::vector<fs::path>& dirs, bool rebuildAll)
{
	for (auto& dir : dirs)
	{
		for (auto& entry : fs::directory_iterator(dir))
		{
			if (entry.is_regular_file())
			{
				fs::path srcPath = entry.path();

				int fileType = Util::index_of(srcPath.extension(), ExtensionForSourceFileType);
				if (fileType < 0)
					continue;

				std::string buildPath = (cfgMgr->build / srcPath).string();
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
					.build = buildSrc
				};
				srcFiles.push_back(srcFile);
			}
		}
	}
}

void NCMaker::pushBuildFiles(std::vector<SourceFile>& srcFiles)
{
	// Fetch dependencies to prevent multiple builds

	std::unordered_map<std::string, fs::file_time_type> timeForDep;

	for (SourceFile& srcFile : srcFiles)
	{
		if (srcFile.build)
			continue;

		if (!fs::exists(srcFile.dep))
		{
			srcFile.build = true;
			continue;
		}

		std::vector<fs::path> deps;

		std::ifstream depStrm(srcFile.dep);
		if (!depStrm.is_open())
		{
			srcFile.build = true;
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
				srcFile.build = true;
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
				srcFile.build = true;
				continue;
			}
		}
	}

	// Save the whole build to memory

	for (SourceFile& srcFile : srcFiles)
	{
		std::string objS = srcFile.obj.string();
		std::string objQ = '\"' + objS + '\"';

		objects += objQ + ' ';

		if (!srcFile.build)
			continue;

		BuildSourceFile& bSrcFile = bSrcFiles.emplace_back();
		bSrcFile.buildDir = srcFile.obj.parent_path();
		std::string srcS = srcFile.src.string();
		std::string srcQ = '\"' + srcS + '\"';

		std::ostringstream bMsgStrm;
		bMsgStrm << OBUILD << "Building " << ObjNameTypeForSourceFileType[srcFile.type] << " object " << OSTR(srcS) << " -> " << OSTR(objS);
		bSrcFile.buildMsg = bMsgStrm.str();

		bSrcFile.buildCmd = cfgMgr->prefix + CompilerForSourceFileType[srcFile.type] + flagsForType[srcFile.type] + srcQ + " -o " + objQ;
	}
}

void NCMaker::generateBuildCache(BuildCache& buildData)
{
	fs::path paths[3] = { build_config, cfgMgr->linker, cfgMgr->symbols };
	std::time_t times[3];

	for (int i = 0; i < 3; i++)
	{
		fs::path& path = paths[i];
		std::time_t& time = times[i];

		std::error_code error;
		fs::file_time_type ftime = fs::last_write_time(path, error);
		if (error)
		{
			// If date can't be obtained, force rebuild.
			time = std::numeric_limits<std::time_t>::max();
		}
		else
		{
			time = Util::to_time_t(ftime);
		}
	}

	buildData.buildConfTime = times[0];
	buildData.linkerTime = times[1];
	buildData.symbolsTime = times[2];
}

bool NCMaker::loadBuildCache(BuildCache& buildData)
{
	fs::path path = fs::path(cfgMgr->build) / build_cache;

	std::ifstream file(path, std::ios::binary);
	if (file.is_open())
	{
		file.read(reinterpret_cast<char*>(&buildData), sizeof(BuildCache));
		file.close();
		return true;
	}
	return false;
}

bool NCMaker::saveBuildCache(BuildCache& buildData)
{
	fs::path path = fs::path(cfgMgr->build) / build_cache;

	std::ofstream file(path, std::ios::binary);
	if (file.is_open())
	{
		file.write(reinterpret_cast<char*>(&buildData), sizeof(BuildCache));
		file.close();
		return true;
	}
	return false;
}

void NCMaker::load()
{
	rapidjson::Document buildConf;
	std::string includeFlags;
	std::vector<SourceFile> srcFiles;
	BuildCache lastBuildData;
	bool rebuildAll;
	bool linkerFilesChanged = false;

	std::cout << OINFO << "Loading build..." << std::endl;

	fs::current_path(workDir);

	if (cfgMgr->sourceDirs.size() == 0)
	{
		throw NC::exception(CompileErr, "Could not find valid source directories to compile.");
	}

	for (const fs::path& include : cfgMgr->includeDirs)
		includeFlags += "-I\"" + include.string() + "\" ";

	for (int i = 0; i < 2; i++)
		flagsForType[i] = includeFlags;

	flagsForType[SourceFileType::C] += cfgMgr->cFlags;
	flagsForType[SourceFileType::Cpp] += cfgMgr->cFlags + cfgMgr->cxxFlags;
	flagsForType[SourceFileType::ASM] += cfgMgr->asFlags;

	ldFlags = cfgMgr->ldFlags;

	generateBuildCache(buildData);
	if (loadBuildCache(lastBuildData))
	{
		rebuildAll = buildData.buildConfTime > lastBuildData.buildConfTime;
		linkerFilesChanged |= buildData.linkerTime > lastBuildData.linkerTime;
		linkerFilesChanged |= buildData.symbolsTime > lastBuildData.symbolsTime;
	}
	else
	{
		rebuildAll = true;
		linkerFilesChanged = true;
	}

	getSrcFiles(srcFiles, cfgMgr->sourceDirs, rebuildAll);

	if (srcFiles.size() == 0)
	{
		throw NC::exception(CompileErr, "Could not find source files to compile.");
	}

	pushBuildFiles(srcFiles);

	ldFlags += "-T\"" + cfgMgr->linker + "\" ";
	ldFlags += "-T\"" + cfgMgr->symbols + "\" ";

	elf = fs::path(cfgMgr->build) / (cfgMgr->target + ".elf");
	bin = fs::path(cfgMgr->build) / (cfgMgr->target + ".bin");
	sym = fs::path(cfgMgr->build) / (cfgMgr->target + ".sym");

	forceRemakeElf = !fs::exists(elf) || !fs::exists(bin) || !fs::exists(sym) || linkerFilesChanged;
}

void NCMaker::buildSource(bool& failed, std::mutex& mtx, const BuildSourceFile& src)
{
	std::ostringstream out;

	out << src.buildMsg << std::endl;
	try
	{
		runProcess(src.buildCmd, &out);
	}
	catch (const NC::exception& e)
	{
		failed = true;
		out << e.what() << std::endl;
	}

	{
		std::lock_guard<std::mutex> lock(mtx);
		std::cout << out.str();
	}
}

bool NCMaker::make()
{
	//std::cout << OINFO << "Executing build..." << std::endl;

	if (bSrcFiles.size() == 0 && !forceRemakeElf)
	{
		std::cout << OBUILD << "All up to date, no need to make!" << std::endl;
		goto make_end;
	}
	else
	{
		// Compile the files

		for (const auto& src : bSrcFiles)
		{
			if (!fs::exists(src.buildDir))
			{
				bool created = fs::create_directories(src.buildDir);
				if (!created)
				{
					std::ostringstream oss;
					oss << "Could not create object directory: " << OSTR(src.buildDir.string());
					throw NC::exception(CompileErr, oss.str());
				}
			}
		}

		bool failed = false;
		std::vector<std::thread> threads;

		for (const auto& src : bSrcFiles)
			threads.push_back(std::thread(buildSource, std::ref(failed), std::ref(mtx), std::ref(src)));

		for (auto& th : threads)
			th.join();

		if (failed)
			return false;
	}

	{
		// Link the files

		std::string elfS = elf.string();
		std::string elfQ = '\"' + elfS + '\"';

		std::cout << OBUILD << "Linking objects -> " << OSTR(elf.string()) << std::endl;

		std::stringstream arenaLoStrm;
		arenaLoStrm << "-Ttext " << std::hex << codeAddr << " ";
		std::string arenaLoStr(arenaLoStrm.str());

		std::string elfCmd = cfgMgr->prefix + "ld " + ldFlags + arenaLoStr + objects + "-o " + elfQ;
		runProcess(elfCmd, &std::cout);

		// Get binary file

		std::string binS = bin.string();
		std::string binQ = '\"' + binS + '\"';

		std::cout << OBUILD << "Making binary " << OSTR(elfS) << " -> " << OSTR(binS) << std::endl;

		std::string binCmd = cfgMgr->prefix + "objcopy -O binary " + elfQ + " " + binQ;
		runProcess(binCmd, nullptr);

		// Get symbols file

		std::string symS = sym.string();
		std::string symQ = '\"' + symS + '\"';

		std::cout << OBUILD << "Dumping symbols " << OSTR(elfS) << " -> " << OSTR(symS) << std::endl;

		std::ofstream symF(sym);
		if(!symF.is_open())
		{
			throw NC::file_error(CompileErr, sym, NC::file_error::write);
		}

		std::string symCmd = cfgMgr->prefix + "nm " + elfQ;

		try
		{
			runProcess(symCmd, &symF);
		}
		catch (const NC::exception& e)
		{
			symF.close();
			throw e;
		}

		symF.close();
	}

make_end:

	saveBuildCache(buildData);
	std::cout << OINFO << "Build finished!" << std::endl;

	return true;
}
