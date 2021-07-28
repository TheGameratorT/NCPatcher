#include "common.hpp"

const char* __ncp_OERROR = OSQRTBRKTS(ANSI_bWHITE, ANSI_bRED, "Error") " ";
const char* __ncp_OWARN = OSQRTBRKTS(ANSI_bWHITE, ANSI_bYELLOW, "Warn") " ";
const char* __ncp_OINFO = OSQRTBRKTS(ANSI_bWHITE, ANSI_bBLUE, "Info") " ";
const char* __ncp_OBUILD = OSQRTBRKTS(ANSI_bWHITE, ANSI_bGREEN, "Build") " ";
const char* __ncp_OREASON = "   -->  ";

int exec(const char* cmd, std::ostream& out)
{
	std::array<char, 128> buffer;
	FILE* pipe = popen(cmd, "r");
	if (!pipe) {
		throw std::runtime_error("popen() failed!");
	}
	while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
		out << buffer.data();
	}
	return pclose(pipe);
}

namespace util
{
	int addr_to_int(const std::string& in)
	{
		return in.starts_with("0x") ? std::stoi(&in[2], nullptr, 16) : std::stoi(in, nullptr, 10);
	}

	std::string int_to_addr(int in, int align)
	{
		std::ostringstream oss;
		oss << "0x";
		if (align != 0)
			oss << std::setfill('0') << std::setw(align);
		oss << std::uppercase << std::hex << in;
		return oss.str();
	}
}
