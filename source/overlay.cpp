#include "overlay.hpp"

#include <fstream>

#include "blz.hpp"
#include "except.hpp"

namespace fs = std::filesystem;

Overlay::Overlay(const fs::path& path, u32 ramAddress, bool compressed)
	: ramAddress(ramAddress)
{
	if (!fs::exists(path))
		throw ncp::file_error(path, ncp::file_error::find);

	uintmax_t fileSize = fs::file_size(path);

	std::ifstream file(path, std::ios::binary);
	if (!file.is_open())
		throw ncp::file_error(path, ncp::file_error::read);

	bytes.resize(fileSize);
	file.read(reinterpret_cast<char*>(bytes.data()), fileSize);
	file.close();

	if (compressed)
		BLZ::uncompress(bytes);
}

Overlay::~Overlay()
{
	
}
