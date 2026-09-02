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
// Because the name table is the ROM's own structure, the numbering is too: a
// member has a file id, assigned by this archive's `BTNF` exactly as the ROM's
// FNT assigns one to a loose file, and game code loads a member by that id.
// Only the table it belongs to differs.
//
// Replace-only, deliberately. Adding or removing an inner file renumbers the
// rest, and those ids are exactly as load-bearing as the ROM's own. Replacing
// keeps every id where it is, which is the same invariant that governs NitroFS
// insertion.
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

#include <optional>

#include "nitro_fs.hpp"
#include "../utils/types.hpp"

namespace ncp::rom {

// True when `data` opens with the NARC magic. Cheap enough to ask before
// parsing, so a caller can say "that file is not an archive" rather than
// reporting a malformed one.
[[nodiscard]] bool isNarc(std::span<const u8> data);

// What an archive was wrapped in when it was read.
//
// Some games store their archives compressed -- Mario Kart DS stores 286 of
// them, named `.carc` -- and a game that reads an archive through its
// decompressor will not accept a raw one in its place. So the wrapper is part of
// the file's identity rather than a size optimisation to be re-decided on the
// way out: an archive that arrived compressed goes back compressed, even where
// that makes it larger.
enum class Wrapper
{
	None,
	Lz10,
	Lz11
};

// The wrapper `data` carries, or nothing when it is not an archive at all.
//
// Decided by what is inside, never by the file's name. `.carc` is a Mario Kart
// naming convention and not a format, so the question asked here is "does this
// decompress to something beginning with NARC" -- which means a game that ships
// compressed archives named `.narc`, or plain ones named `.carc`, needs no
// special case, and `dwc/utility.bin`, which begins with 0x10 and is not
// compressed at all, is not mistaken for one.
[[nodiscard]] std::optional<Wrapper> narcWrapper(std::span<const u8> data);

class Narc
{
public:
	// Unwraps a compressed container before reading it, and records what it
	// was wrapped in so serialize() can put it back the same way.
	static Narc parse(std::span<const u8> data);
	[[nodiscard]] std::vector<u8> serialize() const;

	[[nodiscard]] Wrapper wrapper() const { return m_wrapper; }

	[[nodiscard]] std::size_t fileCount() const { return m_files.size(); }

	// Looks up a '/'-separated path inside the archive. Returns the member's
	// file id in this archive's table, or -1. A nameless archive (BTNF with
	// nothing but a root) never resolves anything, which is correct: its
	// members have ids and no names.
	[[nodiscard]] int findFile(std::string_view path) const;

	[[nodiscard]] std::span<const u8> file(std::size_t index) const;
	void replaceFile(std::size_t index, std::vector<u8> data);

	// Every named member, as (file id, '/'-separated path), sorted by id. Used
	// to list what an archive actually holds when a lookup failed.
	[[nodiscard]] std::vector<std::pair<u32, std::string>> allFiles() const;

private:
	// The BTNF payload exactly as read, and the tree parsed out of it for
	// lookups. The bytes are what gets written back; the tree is never
	// serialized.
	std::vector<u8> m_names;
	NitroFs m_tree;

	std::vector<std::vector<u8>> m_files;

	Wrapper m_wrapper = Wrapper::None;
};

} // namespace ncp::rom
