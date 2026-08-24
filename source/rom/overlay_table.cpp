#include "overlay_table.hpp"

#include <sstream>

#include "endian.hpp"
#include "../system/except.hpp"

namespace ncp::rom {

OverlayEntry OverlayEntry::parse(std::span<const u8> row)
{
	OverlayEntry entry;
	entry.overlayId       = readU32(row, 0x00);
	entry.ramAddress      = readU32(row, 0x04);
	entry.ramSize         = readU32(row, 0x08);
	entry.bssSize         = readU32(row, 0x0C);
	entry.staticInitStart = readU32(row, 0x10);
	entry.staticInitEnd   = readU32(row, 0x14);
	entry.fileId          = readU32(row, 0x18);
	entry.compressedSize  = readU24(row, 0x1C);
	entry.flags           = readU8(row, 0x1F);
	return entry;
}

void OverlayEntry::serialize(std::span<u8> row) const
{
	writeU32(row, 0x00, overlayId);
	writeU32(row, 0x04, ramAddress);
	writeU32(row, 0x08, ramSize);
	writeU32(row, 0x0C, bssSize);
	writeU32(row, 0x10, staticInitStart);
	writeU32(row, 0x14, staticInitEnd);
	writeU32(row, 0x18, fileId);
	writeU24(row, 0x1C, compressedSize);
	writeU8(row, 0x1F, flags);
}

OverlayTable OverlayTable::parse(std::span<const u8> data)
{
	if (data.size() % OverlayEntry::SIZE != 0)
	{
		std::ostringstream oss;
		oss << "Invalid overlay table: " << data.size() << " bytes is not a whole number of "
		    << OverlayEntry::SIZE << "-byte entries.";
		throw ncp::exception(oss.str());
	}

	OverlayTable table;
	const std::size_t count = data.size() / OverlayEntry::SIZE;
	table.m_entries.reserve(count);
	for (std::size_t i = 0; i < count; i++)
		table.m_entries.push_back(OverlayEntry::parse(data.subspan(i * OverlayEntry::SIZE, OverlayEntry::SIZE)));
	return table;
}

std::vector<u8> OverlayTable::serialize() const
{
	std::vector<u8> out(byteSize());
	for (std::size_t i = 0; i < m_entries.size(); i++)
		m_entries[i].serialize(std::span<u8>(out).subspan(i * OverlayEntry::SIZE, OverlayEntry::SIZE));
	return out;
}

} // namespace ncp::rom
