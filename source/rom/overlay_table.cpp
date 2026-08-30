#include "overlay_table.hpp"

#include <sstream>

#include "../utils/endian.hpp"
#include "../system/except.hpp"

namespace ncp::rom {

OverlayEntry OverlayEntry::parse(std::span<const u8> row)
{
	OverlayEntry entry;
	entry.overlayId       = le::readU32(row, 0x00);
	entry.ramAddress      = le::readU32(row, 0x04);
	entry.ramSize         = le::readU32(row, 0x08);
	entry.bssSize         = le::readU32(row, 0x0C);
	entry.staticInitStart = le::readU32(row, 0x10);
	entry.staticInitEnd   = le::readU32(row, 0x14);
	entry.fileId          = le::readU32(row, 0x18);
	entry.compressedSize  = le::readU24(row, 0x1C);
	entry.flags           = le::readU8(row, 0x1F);
	return entry;
}

void OverlayEntry::serialize(std::span<u8> row) const
{
	le::writeU32(row, 0x00, overlayId);
	le::writeU32(row, 0x04, ramAddress);
	le::writeU32(row, 0x08, ramSize);
	le::writeU32(row, 0x0C, bssSize);
	le::writeU32(row, 0x10, staticInitStart);
	le::writeU32(row, 0x14, staticInitEnd);
	le::writeU32(row, 0x18, fileId);
	le::writeU24(row, 0x1C, compressedSize);
	le::writeU8(row, 0x1F, flags);
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
