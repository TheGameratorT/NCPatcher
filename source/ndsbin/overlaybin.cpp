#include "overlaybin.hpp"

#include <cstring>
#include <fstream>

#include "../blz.hpp"
#include "../except.hpp"

namespace fs = std::filesystem;

OverlayBin::OverlayBin() = default;

void OverlayBin::load(const fs::path& path, u32 ramAddress, bool compressed, int id)
{
	m_ramAddress = ramAddress;
	m_id = id;

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

void OverlayBin::readBytes(u32 address, void* out, u32 size) const
{
	u32 binAddress = address - m_ramAddress;
	if (binAddress + size > m_bytes.size())
	{
		std::ostringstream oss;
		oss << "Failed to read from overlay " << m_id << ", reading " << size << " byte(s) from address 0x" <<
			std::uppercase << std::hex << address << std::nouppercase << " exceeds range.";
		throw std::out_of_range(oss.str());
	}
	std::memcpy(out, &m_bytes[binAddress], size);
}

void OverlayBin::writeBytes(u32 address, const void* data, u32 size)
{
	u32 binAddress = address - m_ramAddress;
	if (binAddress + size > m_bytes.size())
	{
		std::ostringstream oss;
		oss << "Failed to write to overlay " << m_id << ", writing " << size << " byte(s) to address 0x" <<
			std::uppercase << std::hex << address << std::nouppercase << " exceeds range.";
		throw std::out_of_range(oss.str());
	}
	std::memcpy(&m_bytes[binAddress], data, size);
}
