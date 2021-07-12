#pragma once

#include <string>
#include <filesystem>

#include "ndsheader.hpp"
#include "common.hpp"

namespace ncp
{
	extern std::filesystem::path asm_path;
	extern std::filesystem::path rom_path;
	extern NDSHeader header_bin;

	const std::string& getErrorMsg();
	void setErrorMsg(const std::string& msg);
}
