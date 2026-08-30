#include "fat.hpp"

#include <sstream>

#include "../utils/endian.hpp"
#include "../system/except.hpp"

namespace ncp::rom {

Fat Fat::parse(std::span<const u8> data)
{
	if (data.size() % FatEntry::SIZE != 0)
	{
		std::ostringstream oss;
		oss << "Invalid file allocation table: " << data.size()
		    << " bytes is not a whole number of " << FatEntry::SIZE << "-byte entries.";
		throw ncp::exception(oss.str());
	}

	Fat fat;
	const std::size_t count = data.size() / FatEntry::SIZE;
	fat.m_entries.resize(count);
	for (std::size_t i = 0; i < count; i++)
	{
		fat.m_entries[i].start = le::readU32(data, i * FatEntry::SIZE);
		fat.m_entries[i].end   = le::readU32(data, i * FatEntry::SIZE + 4);
	}
	return fat;
}

std::vector<u8> Fat::serialize() const
{
	std::vector<u8> out(byteSize());
	std::span<u8> span(out);
	for (std::size_t i = 0; i < m_entries.size(); i++)
	{
		le::writeU32(span, i * FatEntry::SIZE, m_entries[i].start);
		le::writeU32(span, i * FatEntry::SIZE + 4, m_entries[i].end);
	}
	return out;
}

u32 Fat::add(FatEntry entry)
{
	m_entries.push_back(entry);
	return u32(m_entries.size() - 1);
}

} // namespace ncp::rom
