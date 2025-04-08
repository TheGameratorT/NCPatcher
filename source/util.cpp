#include "util.hpp"

#include <iomanip>
#include <sstream>

#include "log.hpp"

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

void printDataAsHex(const void* data, std::size_t size, std::size_t rowlen)
{
	const auto* cdata = static_cast<const unsigned char*>(data);
	for (std::size_t i = 0, j = 0; i < size; i++)
	{
		Log::out << std::setw(2) << std::setfill('0') << std::uppercase << std::hex << int(cdata[i]) << std::nouppercase;
		bool isLastRowByte = j++ > rowlen;
		if (!isLastRowByte)
			Log::out << ' ';
		if (isLastRowByte || i == (size - 1))
		{
			j = 0;
			Log::out << '\n';
		}
	}
	Log::out << std::flush;
}

std::filesystem::path relativeIfSubpath(const std::filesystem::path& path)
{
    try
	{
        auto relative = std::filesystem::relative(path);
		bool notSubpath = relative.string().starts_with("..");

        return notSubpath ? path : std::filesystem::relative(path);
    }
	catch (const std::filesystem::filesystem_error&)
	{
        return path; // Assume it's not a subpath if an error occurs
    }
}

}
