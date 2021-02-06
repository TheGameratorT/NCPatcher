#include "NDSHeader.hpp"

#include <iostream>
#include <fstream>
#include <cstring>

#include "NCException.hpp"

namespace fs = std::filesystem;

static const char* OpenErr = "Could not open the ROM header file.";
static const char* ReadErr = "Could not read the ROM header.";

NDSHeader::NDSHeader(const std::filesystem::path& path)
{
	std::ifstream headerFile(path, std::ios::binary);
	if (!headerFile.is_open())
		throw NC::file_error(OpenErr, path, NC::file_error::read);

	uintmax_t headerSize = std::filesystem::file_size(path);
	if (headerSize < 512)
	{
		headerFile.close();

		std::ostringstream oss;
		oss << "Invalid ROM header file: " << OSTR(path) << "\n";
		oss << "         Expected a minimum of 512 bytes, got " << headerSize << " bytes.";
		throw NC::exception(ReadErr, oss.str());
	}

	headerFile.read(reinterpret_cast<char*>(this), sizeof(NDSHeader));
	headerFile.close();
}
