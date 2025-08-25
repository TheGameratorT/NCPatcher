#include "headerbin.hpp"

#include <iostream>
#include <fstream>
#include <cstring>
#include <sstream>

#include "../app/application.hpp"
#include "../system/log.hpp"
#include "../system/except.hpp"

namespace fs = std::filesystem;

static const char* LoadErr = "Could not load the ROM header.";

HeaderBin::HeaderBin() = default;

void HeaderBin::load(const fs::path& path)
{
	ncp::Application::setErrorContext(LoadErr);

	Log::info("Loading header file...");

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

	// TODO: More safety on HeaderBin loading
	headerFile.read(reinterpret_cast<char*>(this), sizeof(HeaderBin));
	headerFile.close();
}
