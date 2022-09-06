#include "overlaybin.hpp"

#include <fstream>

#include "../blz.hpp"
#include "../except.hpp"

namespace fs = std::filesystem;

OverlayBin::OverlayBin() = default;

void OverlayBin::load(const fs::path& path, u32 ramAddress, bool compressed)
{
	m_ramAddress = ramAddress;

	if (!fs::exists(path))
		throw ncp::file_error(path, ncp::file_error::find);

	uintmax_t fileSize = fs::file_size(path);

	std::ifstream file(path, std::ios::binary);
	if (!file.is_open())
		throw ncp::file_error(path, ncp::file_error::read);

	m_bytes.resize(fileSize);
	file.read(reinterpret_cast<char*>(m_bytes.data()), std::streamsize(fileSize));
	file.close();

	if (compressed)
		BLZ::uncompressInplace(m_bytes);
}
