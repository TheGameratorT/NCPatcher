#pragma once

#include <string>
#include <vector>
#include <filesystem>

class BuildTarget
{
public:
	enum class Mode
	{
		append = 0,
		replace,
		create
	};

	struct Region
	{
		std::vector<std::filesystem::path> sources;
		std::string destination;
		Mode mode;
		bool compress;
		std::string cFlags;
		std::string cppFlags;
		std::string asmFlags;
		std::string ldFlags;
	};

public:
	int arenaLo;
	std::vector<std::filesystem::path> includes;
	std::vector<Region> regions;
	std::filesystem::path symbols;
	std::filesystem::path build;

	BuildTarget();
};
