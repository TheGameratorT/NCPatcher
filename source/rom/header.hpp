#pragma once

// The DS cartridge header, as documented by GBATEK.
//
// Held as the original bytes with typed accessors reading and writing through
// them, rather than as a struct the file is read into. That is not only about
// endianness and padding: the header has regions this tool has no business
// modeling (the DSi extension past 0x180, the Nintendo logo, the secure-area
// checksum of an encrypted region we never touch) and a header that is parsed
// into fields and written back out again loses every one of them. Patching the
// handful of fields a code patch actually moves keeps the rest byte-identical.

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "../utils/types.hpp"

namespace ncp::rom {

// Where a binary lives, both in the ROM and in memory.
struct ArmBinaryInfo
{
	u32 romOffset = 0;
	u32 entryAddress = 0;
	u32 ramAddress = 0;
	u32 size = 0;
};

// A plain ROM extent: the tables, the banner, the debug binary.
struct RomRegion
{
	u32 romOffset = 0;
	u32 size = 0;

	[[nodiscard]] constexpr u32 end() const { return romOffset + size; }
};

class Header
{
public:
	// Every retail cartridge header is 0x200 bytes; DSi titles extend it to
	// 0x1000. Anything shorter is not a header.
	static constexpr std::size_t SIZE = 0x200;
	static constexpr std::size_t DSI_SIZE = 0x1000;

	// Offset of the last byte covered by headerChecksum, i.e. the checksum is
	// over [0, CHECKSUM_COVERAGE).
	static constexpr std::size_t CHECKSUM_COVERAGE = 0x15E;

	Header() = default;

	// Takes ownership of the bytes as read from the ROM or from header.bin.
	// Trailing padding is kept: an extracted header.bin is often padded out to
	// the full ROM header size, and writing back a shorter file would change
	// what a repack produces.
	void parse(std::vector<u8> bytes);
	void load(const std::filesystem::path& path);

	[[nodiscard]] bool loaded() const { return m_bytes.size() >= SIZE; }
	[[nodiscard]] const std::vector<u8>& bytes() const { return m_bytes; }
	[[nodiscard]] std::span<const u8> span() const { return m_bytes; }

	[[nodiscard]] std::string gameTitle() const;
	[[nodiscard]] std::string gameCode() const;
	[[nodiscard]] std::string makerCode() const;
	[[nodiscard]] u8 unitCode() const;
	[[nodiscard]] u8 romVersion() const;

	// 128 KiB << deviceCapacity. Never shrunk when repacking: the value also
	// tells a flashcart how much to read, and a cartridge that reports less
	// than it holds is worse than one that reports more.
	[[nodiscard]] u8 deviceCapacity() const;
	void setDeviceCapacity(u8 value);
	[[nodiscard]] u32 deviceCapacityBytes() const;

	[[nodiscard]] ArmBinaryInfo arm(bool arm9) const;
	void setArmRomOffset(bool arm9, u32 value);
	void setArmSize(bool arm9, u32 value);

	[[nodiscard]] u32 autoLoadListHookAddress(bool arm9) const;

	[[nodiscard]] RomRegion fnt() const;
	void setFnt(RomRegion region);
	[[nodiscard]] RomRegion fat() const;
	void setFat(RomRegion region);
	[[nodiscard]] RomRegion overlayTable(bool arm9) const;
	void setOverlayTable(bool arm9, RomRegion region);

	[[nodiscard]] u32 bannerOffset() const;
	void setBannerOffset(u32 value);

	[[nodiscard]] u32 totalUsedRomSize() const;
	void setTotalUsedRomSize(u32 value);

	// 0x4000 on a retail cartridge: the header is followed by the secure area,
	// and arm9 starts at this offset rather than right after the 0x200 bytes.
	[[nodiscard]] u32 headerSize() const;

	[[nodiscard]] u16 storedChecksum() const;
	[[nodiscard]] u16 computeChecksum() const;
	// Recomputes the header checksum in place. The logo and secure-area
	// checksums are deliberately left alone, since we never modify what they
	// cover, and recomputing the secure-area one would mean deciding what to do
	// about encryption.
	void updateChecksum();

	// True when the header advertises the DSi extension. Its extra fields are
	// preserved rather than understood.
	[[nodiscard]] bool isDsi() const;

private:
	std::vector<u8> m_bytes;

	[[nodiscard]] std::span<const u8> read() const { return m_bytes; }
	[[nodiscard]] std::span<u8> write() { return m_bytes; }
	[[nodiscard]] std::string text(std::size_t offset, std::size_t length) const;
};

} // namespace ncp::rom
