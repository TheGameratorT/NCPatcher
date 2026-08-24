#pragma once

// The ARM9 / ARM7 overlay tables.
//
// One 32-byte row per overlay, in id order. The last eight bytes are a 24-bit
// compressed size followed by a flags byte, which is exactly the shape a C
// bitfield gets wrong: the bit order within a storage unit is
// implementation-defined, so the previous `u32 compressed : 24; u32 flag : 8;`
// only happened to work on the compilers it had been tried with.

#include <cstddef>
#include <span>
#include <vector>

#include "../utils/types.hpp"

namespace ncp::rom {

struct OverlayEntry
{
	// Serialized size of one row. The overlay count of a table is its size
	// divided by this.
	static constexpr std::size_t SIZE = 32;

	// GBATEK's overlay flag bits.
	static constexpr u8 FlagCompressed = 0x01;
	static constexpr u8 FlagAuthenticated = 0x02;

	u32 overlayId = 0;
	u32 ramAddress = 0;
	u32 ramSize = 0;
	u32 bssSize = 0;
	u32 staticInitStart = 0;
	u32 staticInitEnd = 0;
	u32 fileId = 0;
	u32 compressedSize = 0;   // 24-bit; the size actually stored in the ROM
	u8 flags = 0;

	[[nodiscard]] constexpr bool compressed() const { return (flags & FlagCompressed) != 0; }
	[[nodiscard]] constexpr bool authenticated() const { return (flags & FlagAuthenticated) != 0; }

	constexpr void setCompressed(bool on)
	{
		flags = on ? u8(flags | FlagCompressed) : u8(flags & ~FlagCompressed);
	}

	// How many bytes of this overlay are stored in the ROM. An uncompressed
	// overlay leaves compressedSize at zero and occupies ramSize bytes.
	[[nodiscard]] constexpr u32 storedSize() const
	{
		return (compressed() && compressedSize != 0) ? compressedSize : ramSize;
	}

	static OverlayEntry parse(std::span<const u8> row);
	void serialize(std::span<u8> row) const;
};

class OverlayTable
{
public:
	// Parses a whole table. A size that is not a multiple of the row size is a
	// truncated table rather than a table with a fractional last row.
	static OverlayTable parse(std::span<const u8> data);
	[[nodiscard]] std::vector<u8> serialize() const;

	[[nodiscard]] std::vector<OverlayEntry>& entries() { return m_entries; }
	[[nodiscard]] const std::vector<OverlayEntry>& entries() const { return m_entries; }
	[[nodiscard]] std::size_t size() const { return m_entries.size(); }
	[[nodiscard]] bool empty() const { return m_entries.empty(); }

	[[nodiscard]] std::size_t byteSize() const { return m_entries.size() * OverlayEntry::SIZE; }

private:
	std::vector<OverlayEntry> m_entries;
};

} // namespace ncp::rom
