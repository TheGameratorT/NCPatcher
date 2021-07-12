#pragma once

#include <string>
#include <vector>
#include <filesystem>

class BuildTarget
{
	struct Region
	{
		std::vector<std::filesystem::path> sources;
		std::string destination;
		std::string mode;
		bool compress;
		std::string cFlags;
		std::string cppFlags;
		std::string asmFlags;
	};

public:
	int arenaLo;
	std::vector<std::filesystem::path> includes;
	std::vector<Region> regions;
	std::filesystem::path symbols;

	BuildTarget(const std::filesystem::path& path);
};
