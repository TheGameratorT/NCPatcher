#pragma once

// A Nitro archive: a filesystem inside a file.
//
// The DS keeps most of a game's assets in `.narc` containers rather than as
// loose NitroFS files, and a project that wants to change one of those assets
// has to open the container to reach it. That is the whole reason this exists;
// it is not a general archive tool.
//
// The layout is the ROM's own filesystem in miniature. `BTAF` is a FAT, `BTNF`
// is the same File Name Table structure nitro_fs.cpp already parses and
// serializes, and `GMIF` holds the bytes, which is why the filename half of
// this file is reuse and only the chunk framing and the FAT are new.
//
// Replace-only, deliberately. Adding or removing an inner file renumbers the
// rest, and inner file numbers are exactly as load-bearing as the ROM's own:
// game code reads a NARC member by index. Replacing keeps every index where it
// is, which is the same invariant that governs NitroFS insertion.
//
// The name table is kept as the bytes it was read as and never reserialized.
// Nothing here renames anything, so re-emitting a structure that did not change
// could only introduce a difference, and an archive nobody edited has to come
// back out byte-identical, since that is the cheapest evidence the writer is
// correct.

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "nitro_fs.hpp"
#include "../utils/types.hpp"

namespace ncp::rom {

// True when `data` opens with the NARC magic. Cheap enough to ask before
// parsing, so a caller can say "that file is not an archive" rather than
// reporting a malformed one.
[[nodiscard]] bool isNarc(std::span<const u8> data);

class Narc
{
public:
	static Narc parse(std::span<const u8> data);
	[[nodiscard]] std::vector<u8> serialize() const;

	[[nodiscard]] std::size_t fileCount() const { return m_files.size(); }

	// Looks up a '/'-separated path inside the archive. Returns the member
	// index, or -1. A nameless archive (BTNF with nothing but a root) never
	// resolves anything, which is correct: its members have numbers and no
	// names.
	[[nodiscard]] int findFile(std::string_view path) const;

	[[nodiscard]] std::span<const u8> file(std::size_t index) const;
	void replaceFile(std::size_t index, std::vector<u8> data);

	// Every named member, as (index, '/'-separated path), sorted by index.
	// Used to list what an archive actually holds when a lookup failed.
	[[nodiscard]] std::vector<std::pair<u32, std::string>> allFiles() const;

private:
	// The BTNF payload exactly as read, and the tree parsed out of it for
	// lookups. The bytes are what gets written back; the tree is never
	// serialized.
	std::vector<u8> m_names;
	NitroFs m_tree;

	std::vector<std::vector<u8>> m_files;
};

} // namespace ncp::rom
