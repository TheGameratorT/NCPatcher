#pragma once

#include <vector>
#include <filesystem>

class BuildCfg
{
public:
	std::filesystem::path backup;
	std::filesystem::path arm7;
	std::filesystem::path arm9;

	void load(const std::filesystem::path& path);
};
