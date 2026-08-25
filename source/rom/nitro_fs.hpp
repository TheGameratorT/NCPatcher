#pragma once

// The File Name Table -- NitroFS's directory tree.
//
// A directory table of 8-byte rows, one per directory, followed by one subtable
// per directory listing its children. File ids are implicit: a subtable's files
// are numbered consecutively from the directory row's firstFileId, which is why
// inserting a file renumbers every file after it and why nothing here hands out
// ids on its own.
//
// Directory ids are 0xF000-based; a child row named in a subtable stores the
// full id, and the row's index in the directory table is the low 12 bits.

#include <cstddef>
#include <span>
#include <string>
#include <utility>
#include <string_view>
#include <vector>

#include "../utils/types.hpp"

namespace ncp::rom {

// Directory ids start here; the root is this value exactly.
constexpr u16 FIRST_DIR_ID = 0xF000;

struct FsEntry
{
	std::string name;
	bool isDirectory = false;
	// File id for a file, directory id (0xF000-based) for a directory.
	u16 id = 0;
};

struct FsDirectory
{
	u16 id = FIRST_DIR_ID;
	u16 parentId = FIRST_DIR_ID;
	u16 firstFileId = 0;
	std::vector<FsEntry> entries;
};

class NitroFs
{
public:
	static NitroFs parse(std::span<const u8> data);
	[[nodiscard]] std::vector<u8> serialize() const;

	[[nodiscard]] const std::vector<FsDirectory>& directories() const { return m_directories; }
	[[nodiscard]] bool empty() const { return m_directories.empty(); }

	// The number of files the tree names. Not the FAT's size: the FAT also
	// holds the overlays, which come before the named files and have no entry
	// here.
	[[nodiscard]] std::size_t fileCount() const;

	// Looks up a '/'-separated path. Returns the file id, or -1 when no such
	// file exists.
	[[nodiscard]] int findFile(std::string_view path) const;

	// Looks up a '/'-separated directory path. Returns the directory id, or -1.
	// An empty path is the root.
	[[nodiscard]] int findDirectory(std::string_view path) const;

	// Adds a new path for a file id that was just appended to the FAT. Missing
	// directories are created. Existing ids are never renumbered: if the file
	// cannot be appended to its directory's consecutive id range, this refuses.
	void addFile(std::string_view path, u32 fileId);

	// Gives an existing file id a different name, in the directory that already
	// holds it. Nothing is renumbered and nothing moves: this rewrites one FNT
	// entry's name and no more.
	//
	// It exists because a ROM's file ids are its stable identity -- code and
	// saved data refer to them -- while a project may need a path the retail
	// ROM never had. Appending would allocate a fresh id at the end of the
	// table; renaming an existing one keeps every id where it was. The trade is
	// that the old name is gone, so the caller has to know the file it is
	// repurposing is unused.
	//
	// `path`'s parent directory must be the one currently holding `fileId`,
	// because a file id belongs to its directory's consecutive range and moving
	// it elsewhere is exactly the renumbering this refuses to do.
	void renameFile(u32 fileId, std::string_view path);

	// The '/'-separated path a file id is named by, or empty when the tree does
	// not name it.
	[[nodiscard]] std::string pathOfFile(u32 fileId) const;

	// Every named file, as (id, '/'-separated path), sorted by id.
	//
	// Walking the tree once is what makes a whole-ROM report affordable: the
	// per-id lookup above is a linear scan, so asking it two thousand times to
	// build a manifest would be quadratic for no reason.
	[[nodiscard]] std::vector<std::pair<u32, std::string>> allFiles() const;

	// Names an existing file id inside an existing directory. Used to give an
	// overlay a name so that tools which resolve files by path can see it; the
	// game itself loads overlays by id and never consults this.
	//
	// The id must already be reachable -- this does not renumber anything, and
	// refuses when it would have to.
	void nameFile(u16 directoryId, const std::string& name, u16 fileId);

	// Builds the '/'-separated path of a directory, root first, without a
	// leading slash. Empty for the root itself.
	[[nodiscard]] std::string pathOf(u16 directoryId) const;

private:
	std::vector<FsDirectory> m_directories;

	[[nodiscard]] const FsDirectory* directory(u16 id) const;
	[[nodiscard]] FsDirectory* directory(u16 id);
	[[nodiscard]] u16 ensureDirectory(std::string_view path);
};

} // namespace ncp::rom
