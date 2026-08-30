#include "endian.hpp"

#include <sstream>

namespace ncp::le {

void throwOutOfRange(std::size_t offset, std::size_t size, std::size_t available)
{
	std::ostringstream oss;
	oss << "Data is truncated: needed " << size << " byte(s) at offset 0x"
	    << std::hex << std::uppercase << offset << std::nouppercase << std::dec
	    << ", but only " << available << " byte(s) are present.";
	throw std::out_of_range(oss.str());
}

} // namespace ncp::le
