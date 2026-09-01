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
};

// Folds the ROM's file table together with what was just inserted.
//
// `files` is the resolved insertion list, and `before` is the set of ids that
// already existed when the build started, which is what separates a file this
// run created from one it replaced, since by the time the manifest is written
// both are simply present.
// `missingSources` names the destinations whose source a plan could not read.
// Always empty after a real build, which refuses to start without them.
[[nodiscard]] std::vector<ManifestEntry> buildManifest(
	const RomAccessor& rom,
	const std::vector<config::FileConfig>& files,
	const std::vector<u32>& createdIds,
	const std::filesystem::path& projectRoot,
	const std::vector<std::string>& missingSources = {});

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
