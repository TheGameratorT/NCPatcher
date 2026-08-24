#pragma once

// The File Allocation Table: one { start, end } ROM offset pair per file id,
// in id order. Overlays are files too -- an overlay table row names a file id,
// and this is what turns that into an extent.

#include <cstddef>
#include <span>
#include <vector>

#include "../utils/types.hpp"

namespace ncp::rom {

struct FatEntry
{
	static constexpr std::size_t SIZE = 8;

	u32 start = 0;
	// One past the last byte, so an empty file has start == end. Files are not
	// required to be stored in id order, and a repack must not assume they are.
	u32 end = 0;

	[[nodiscard]] constexpr u32 size() const { return end > start ? end - start : 0; }
};

class Fat
{
public:
	static Fat parse(std::span<const u8> data);
	[[nodiscard]] std::vector<u8> serialize() const;

	[[nodiscard]] std::vector<FatEntry>& entries() { return m_entries; }
	[[nodiscard]] const std::vector<FatEntry>& entries() const { return m_entries; }
	[[nodiscard]] std::size_t size() const { return m_entries.size(); }
	[[nodiscard]] bool empty() const { return m_entries.empty(); }
	[[nodiscard]] std::size_t byteSize() const { return m_entries.size() * FatEntry::SIZE; }

	// Appends an entry and returns the file id it was given.
	u32 add(FatEntry entry);

private:
	std::vector<FatEntry> m_entries;
};

} // namespace ncp::rom
