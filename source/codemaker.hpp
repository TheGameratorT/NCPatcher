#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <mutex>

#include "common.hpp"
#include "buildtarget.hpp"

class CodeMaker
{
public:
	CodeMaker(const BuildTarget& target);

	struct BuiltRegion
	{
		const BuildTarget::Region* region;
		std::vector<std::filesystem::path> objects;
		bool rebuildElf;
	};

	const std::vector<BuiltRegion>& getBuiltRegions() const;

private:
	struct SourceFileType {
		enum {
			C, CPP, ASM
		};
	};

	struct SourceFile
	{
		std::filesystem::path src;
		std::filesystem::path obj;
		std::filesystem::path dep;
		std::filesystem::file_time_type objT;
		size_t type;
		bool rebuild;
	};

	struct BuildSourceFile
	{
		std::filesystem::path src;
		std::filesystem::path obj;
		std::filesystem::path dep;
		std::string flags;
		size_t type;
	};

	const BuildTarget& target;
	std::mutex mtx;

	std::string includeFlags;
	std::vector<BuildSourceFile> buildFiles;
	std::vector<BuiltRegion> builtRegions;
	
	void setup();
	void make();
	void getSrcFiles(std::vector<SourceFile>& srcFiles, const BuildTarget::Region& region, bool rebuildAll);
	void pushBuildFiles(std::vector<SourceFile>& srcFiles, const BuildTarget::Region& region, BuiltRegion& bregion);
	void buildSource(bool& failed, const BuildSourceFile& src);
};
