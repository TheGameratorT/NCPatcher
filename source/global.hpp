#pragma once

#include <string>
#include <filesystem>

#include "buildcfg.hpp"
#include "ndsheader.hpp"
#include "common.hpp"

namespace ncp
{
	extern std::filesystem::path exe_dir;
	extern std::filesystem::path asm_path;
	extern std::filesystem::path rom_path;
	extern BuildCfg build_cfg;
	extern NDSHeader header_bin;

	const std::string& getErrorMsg();
	void setErrorMsg(const std::string& msg);
}
