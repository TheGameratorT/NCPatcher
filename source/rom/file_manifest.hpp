#pragma once

// The ROM's file table, written out for whoever needs to name a file by id.
//
// A DS game refers to its files by number, not by path, so any code that loads
// one needs a constant, and that constant has to be regenerated whenever the
// table changes. The generator cannot read `files:` to work them out, because
// most of the table is whatever the retail ROM already had and only a handful
// of entries come from the project. So the whole table is reported: two
// thousand paths the build did not touch alongside the thirteen it did.
//
// The other consumer is an editor. Every entry says whether it is vanilla,
// replaced by a module, or added by one, and which variant supplied the bytes.
// That is the difference between a file browser and a view of what this project
// changes about the ROM, and grouping by path across variants answers "which
// languages translate this file", which is otherwise a directory crawl.
//
// Ids are raw, exactly as the FAT stores them. A game that offsets file ids at
// run time (NSMB subtracts its overlay count) applies that itself; it is a
// property of that game, not of the ROM.

#include <filesystem>
#include <ostream>
#include <string>
#include <vector>

#include "accessor.hpp"
#include "../config/project_config.hpp"

namespace ncp::rom {

// What this run did to a file, from the point of view of the ROM that came in.
enum class FileAction
{
	Unchanged,
	Modified,
	Created
};

// One member of a Nitro archive that this run replaced.
//
// `id` is a file id, in the container's own table rather than the ROM's. A
// NARC carries the same File Name Table structure a ROM does, and it numbers
// its members the same way -- which is why `narc.cpp` reuses `nitro_fs.cpp` to
// read it. Two tables, two numbering spaces, one concept: an id here addresses
// a member of this archive, an id on a ManifestEntry addresses a file of the
// ROM, and neither is meaningful in the other's table.
//
// So the number is qualified by what holds it, never renamed to hide what it
// is. Game code loads a member by this id exactly as it loads a loose file by
// its own, which means the same argument that makes the ROM's table worth
// dumping -- code needs the constant, and the constant moves when the table
// does -- applies here.
//
// `action` is only ever `Modified`. The archive codec is replace-only, because
// inserting a member renumbers every member after it -- so there is no such
// thing as a created member.
struct ManifestMember
{
	u32 id = 0;

	// '/'-separated path inside the archive, as the container's own name table
	// spells it.
	std::string path;
	u32 size = 0;
	FileAction action = FileAction::Modified;

	std::string source;
	std::string module;
	std::string component;
	std::string fromVariant;
	bool sourceMissing = false;
};

// The manifest's view of one file: the ROM's own facts, plus what the build
// knows about where the bytes came from.
struct ManifestEntry
{
	u32 id = 0;
	std::string path;
	u32 size = 0;
	FileAction action = FileAction::Unchanged;

	// Empty unless this run wrote the file. Relative to the project when the
	// source is inside it, which is how the config wrote it.
	std::string source;
	std::string module;
	std::string component;
	std::string fromVariant;

	// Planning only: the source this entry names was not on disk to be
	// measured, because a hook has not generated it yet. The destination, the
	// id and the provenance are still right; the size is not.
	bool sourceMissing = false;

	// Present only for an archive this run edited, and then only for the
	// members it edited. Enumerating every member of every archive would dwarf
	// the rest of the document, and a consumer holding the ROM can list them
	// itself; what it cannot work out on its own is provenance, so that is
	// what this carries.
	std::vector<ManifestMember> members;
};

// What one member replacement did, before the manifest relativises its source.
struct ArchiveEdit
{
	// The container's own ROM path, which is what the manifest entry this
	// belongs to is keyed by.
	std::string archive;

	// The member's file id inside that container.
	u32 id = 0;
	std::string member;
	u32 size = 0;

	std::filesystem::path source;
	std::string module;
	std::string component;
	std::string fromVariant;
	bool sourceMissing = false;
};

// What one insertion pass did, in the terms the manifest needs to hear it.
//
// `files` is the resolved insertion list, which says where each file's bytes
// came from. `createdIds` is what separates a file this run created from one it
// replaced, since by the time the manifest is written both are simply present.
// `archiveEdits` is what happened inside a container, which the ROM's table
// cannot show, because it does not name members at all.
struct InsertionRecord
{
	std::vector<config::FileConfig> files;
	std::vector<u32> createdIds;

	// Planning only: destinations whose source was not on disk to be measured.
	// Always empty after a real build, which refuses to start without them.
	std::vector<std::string> missingSources;

	std::vector<ArchiveEdit> archiveEdits;
};

// Folds the ROM's file table together with what was just inserted.
[[nodiscard]] std::vector<ManifestEntry> buildManifest(
	const RomAccessor& rom,
	const InsertionRecord& inserted,
	const std::filesystem::path& projectRoot);

// Writes `ncpatcher.files/1`.
//
// `planned` marks a document produced by `files plan` rather than by a build:
// every id under `z_new/` in one is a prediction, and a build is what confirms
// it. A consumer must never mistake the two, which is why the flag is on the
// document rather than left to be inferred from the command that wrote it.
void writeManifest(std::ostream& out,
                   const std::vector<ManifestEntry>& entries,
                   std::string_view variant,
                   bool planned = false);

} // namespace ncp::rom
