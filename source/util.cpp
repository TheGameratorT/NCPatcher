#include "util.hpp"

#include <iomanip>
#include <sstream>

namespace Util {

int addrToInt(const std::string& in)
{
	return in.starts_with("0x") ? std::stoi(&in[2], nullptr, 16) : std::stoi(in, nullptr, 10);
}

std::string intToAddr(int in, int align, bool prefix)
{
	std::ostringstream oss;
	if (prefix)
		oss << "0x";
	if (align != 0)
		oss << std::setfill('0') << std::setw(align);
	oss << std::uppercase << std::hex << in;
	return oss.str();
}

}
