#pragma once

// A .nds cartridge image, held whole in memory.
//
// Holding all of it is a deliberate simplification. The alternative -- seeking
// around an open file -- buys nothing here: a DS ROM is at most 256 MiB and
// usually a fraction of that, every write this tool makes is to a region whose
// neighbours may have to move, and a half-written .nds is not a ROM at all. In
// memory, "grow arm9 and shift everything after it" is a buffer edit rather
// than a careful dance of overlapping reads and writes, and the file is
// replaced atomically at the end.
//
// It also means the bytes this tool does not model -- the secure area, the
// banner, every level and texture, and whatever a dumper left in the padding
// past the end -- survive untouched, because nothing ever rebuilds them from a
// parsed representation.

#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include "fat.hpp"
#include "header.hpp"
#include "nitro_fs.hpp"
#include "overlay_table.hpp"
#include "../utils/types.hpp"

namespace ncp::rom {

// GBATEK: an ARM9 binary may be followed by this 12-byte block. It has to move
// with the binary, so it is tracked rather than treated as part of the gap.
constexpr u32 NITROCODE = 0xDEC00621;
constexpr u32 NITROCODE_FOOTER_SIZE = 12;

class NdsRom
{
public:
	void load(const std::filesystem::path& path);
	void parse(std::vector<u8> bytes);

	// Writes to a temporary beside the destination and renames over it, so an
	// interrupted save cannot leave a truncated ROM where a working one was.
	void save(const std::filesystem::path& path) const;

	[[nodiscard]] const Header& header() const { return m_header; }
	[[nodiscard]] Header& header() { return m_header; }
	[[nodiscard]] const Fat& fat() const { return m_fat; }
	[[nodiscard]] const NitroFs& nitroFs() const { return m_nitroFs; }
	[[nodiscard]] NitroFs& nitroFs() { return m_nitroFs; }
	[[nodiscard]] const OverlayTable& overlayTable(bool arm9) const { return m_ovt[arm9 ? 1 : 0]; }

	[[nodiscard]] const std::vector<u8>& bytes() const { return m_bytes; }

	// Current contents, with any staged replacement already applied.
	[[nodiscard]] std::vector<u8> readArm(bool arm9) const;
	[[nodiscard]] std::vector<u8> readFile(u32 fileId) const;
	[[nodiscard]] bool hasFile(u32 fileId) const;

	// The size a file has now, staged writes included. Separate from readFile
	// because the FAT does not learn a staged file's size until commit() places
	// it, and because listing the whole table has no business copying sixty
	// megabytes of file contents to ask how long each one is.
	[[nodiscard]] u32 fileSize(u32 fileId) const;

	// The icon/title banner, as a whole region. Its length is fixed by the
	// version word it starts with, so a replacement has to be the same size --
	// growing it would mean moving whatever follows it, and a banner is not
	// worth laying the container out again for.
	[[nodiscard]] std::vector<u8> readBanner() const;
	[[nodiscard]] bool hasBanner() const { return bannerSize() != 0; }
	void setBanner(std::vector<u8> data);

	void setArm(bool arm9, std::vector<u8> data);
	void setOverlayTable(bool arm9, OverlayTable table);
	void setFile(u32 fileId, std::vector<u8> data);

	// Replaces the file name table. Kept as a staged write rather than
	// reserialized on every commit, so that a ROM whose tree nobody touched
	// keeps the exact FNT bytes it shipped with.
	void setNitroFs(const NitroFs& tree);

	// Appends a file to the FAT and returns its id. The bytes are placed by
	// commit() like any other staged write.
	u32 addFile(std::vector<u8> data);

	// True once anything has been staged. A commit with nothing staged leaves
	// the file alone entirely, rather than rewriting it identically.
	[[nodiscard]] bool dirty() const;

	// Applies every staged write. `arm9Slack` is how many spare bytes to leave
	// after the ARM9 binary when the ROM has to be laid out again; see the
	// comment on rebuildLayout in the implementation for why that matters.
	void commit(u32 arm9Slack);

	// Whether the last commit() had to move regions around. Reported rather
	// than hidden: a rebuild changes every file offset in the ROM, which is
	// worth a line in the log.
	[[nodiscard]] bool lastCommitRebuilt() const { return m_lastCommitRebuilt; }

private:
	std::vector<u8> m_bytes;
	Header m_header;
	Fat m_fat;
	NitroFs m_nitroFs;
	OverlayTable m_ovt[2];   // [0] arm7, [1] arm9

	std::optional<std::vector<u8>> m_pendingArm[2];
	std::optional<OverlayTable> m_pendingOvt[2];
	std::map<u32, std::vector<u8>> m_pendingFiles;
	std::optional<std::vector<u8>> m_pendingFnt;
	std::optional<std::vector<u8>> m_pendingBanner;
	bool m_lastCommitRebuilt = false;

	[[nodiscard]] std::span<const u8> region(const RomRegion& region, const char* what) const;
	[[nodiscard]] std::vector<u8> slice(u32 offset, u32 size) const;
	[[nodiscard]] bool hasNitrocodeFooter() const;
	[[nodiscard]] u32 bannerSize() const;

	// Every start offset the ROM currently has something at, sorted. Used to
	// work out how much room a region has before the next one begins.
	[[nodiscard]] std::vector<u32> occupiedStarts() const;
	[[nodiscard]] u32 roomAfter(u32 start, const std::vector<u32>& starts) const;

	void rebuildLayout(u32 arm9Slack);
	void writeRegion(u32 offset, std::span<const u8> data);
};

} // namespace ncp::rom
