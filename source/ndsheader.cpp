#include "ndsheader.hpp"

#include <iostream>
#include <fstream>
#include <cstring>

#include "except.hpp"
#include "global.hpp"
#include "oansistream.hpp"

namespace fs = std::filesystem;

static const char* LoadErr = "Could not load the ROM header.";

void NDSHeader::load(const fs::path& path)
{
	ncp::setErrorMsg(LoadErr);

	ansi::cout << OINFO << "Loading header file..." << std::endl;

	if (!fs::exists(path))
		throw ncp::file_error(path, ncp::file_error::find);
	
	std::ifstream headerFile(path, std::ios::binary);
	if (!headerFile.is_open())
		throw ncp::file_error(path, ncp::file_error::read);

	uintmax_t headerSize = fs::file_size(path);
	if (headerSize < 512)
	{
		headerFile.close();

		std::ostringstream oss;
		oss << "Invalid ROM header file: " << OSTR(path.string()) << OREASONNL;
		oss << "Expected a minimum of 512 bytes, got " << headerSize << " bytes.";
		throw ncp::exception(oss.str());
	}

	headerFile.read(reinterpret_cast<char*>(this), sizeof(NDSHeader));
	headerFile.close();
}
