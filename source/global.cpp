#include "global.hpp"

namespace ncp
{
	std::filesystem::path asm_path;
	std::filesystem::path rom_path;
	NDSHeader header_bin;
	
	std::string _errorMsg;

	const std::string& getErrorMsg() { return _errorMsg; }
	void setErrorMsg(const std::string& msg) { _errorMsg = msg; }
}
