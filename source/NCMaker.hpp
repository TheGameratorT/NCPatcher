#pragma once

#include <filesystem>
#include <string>
#include <vector>
#include <mutex>

#include "ConfigManager.hpp"
#include "Common.hpp"

class NCMaker
{
public:
	NCMaker(const std::filesystem::path& path, const ConfigManager& cfgMgr, u32 codeAddr);
	void load();
	bool make();

private:
	std::filesystem::path workDir;
	std::filesystem::path elf;
	std::filesystem::path bin;
	std::filesystem::path sym;
	std::string objects;
	bool forceRemakeElf;

	u32 codeAddr;
	const ConfigManager* cfgMgr;

	std::string flagsForType[3];
	std::string ldFlags;

	std::mutex mtx;

	struct BuildCache
	{
		std::time_t buildConfTime;
		std::time_t linkerTime;
		std::time_t symbolsTime;
	};

	struct SourceFileType
	{
		enum
		{
			C, Cpp, ASM
		};
	};

	struct SourceFile
	{
		std::filesystem::path src;
		std::filesystem::path obj;
		std::filesystem::path dep;
		std::filesystem::file_time_type objT;
		int type;
		bool build;
	};

	struct BuildSourceFile
	{
		std::filesystem::path buildDir;
		std::string buildMsg;
		std::string buildCmd;
	};

	BuildCache buildData;
	std::vector<BuildSourceFile> bSrcFiles;

	static void runProcess(const std::string& cmd, std::ostream* out);
	void getSrcFiles(std::vector<SourceFile>& srcFiles, const std::vector<std::filesystem::path>& dirs, bool rebuildAll);
	void pushBuildFiles(std::vector<SourceFile>& srcFiles);
	void generateBuildCache(BuildCache& buildData);
	bool loadBuildCache(BuildCache& buildData);
	bool saveBuildCache(BuildCache& buildData);

	static void buildSource(bool& failed, std::mutex& mtx, const BuildSourceFile& src);
};
