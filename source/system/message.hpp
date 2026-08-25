#pragma once

// The machine-readable half of the output.
//
// Everything a person reads goes through Log. This is the other audience: a
// build system, a CI job, or the level editor, none of which should ever have
// to match on English. With --message-format json each of these calls emits one
// self-contained JSON object on its own line on stdout, while the human log
// moves to stderr; without it they are almost free no-ops, and the human output
// is unchanged.
//
// The event that matters most is `artifact`. NSMB-Editor currently crashes when
// NCPatcher creates an overlay, because the editor re-imports the patched
// directory and looks the new overlay up by a name its filesystem has never
// heard of. Reporting what was created, with its id and RAM address, is what
// lets the caller add it instead of guessing.

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

#include "diagnostics.hpp"
#include "../utils/types.hpp"

namespace ncp::msg {

enum class Format
{
	Human,
	Json
};

enum class Level
{
	Error,
	Warning,
	Note
};

// Where in a configuration file something is. An empty `file` means the
// diagnostic is not about a place in a file, which is the common case.
struct Location
{
	std::string file;
	int line = 0;
	int column = 0;
	// The node path, e.g. "targets.arm9.regions[12].maxsize".
	std::string path;

	[[nodiscard]] bool valid() const { return !file.empty(); }
};

// Something this run produced or changed in the ROM.
struct Artifact
{
	std::string kind;   // "arm", "overlay", "overlay-table", "file", "archive-file", "banner"
	std::string proc;   // "arm9" or "arm7"
	std::string action; // "modified", "created", "restored"
	std::string name;   // the file name inside the ROM directory

	int id = -1;              // overlay id, or -1 where the kind has none
	long long size = -1;      // bytes, or -1 if unknown
	u32 ramAddress = 0;
	bool hasRamAddress = false;
	int fileId = -1;
};

// Chooses the format and, optionally, a file to write the run summary to.
// Also starts the clock the result event reports.
void configure(Format format, std::filesystem::path resultFile);

[[nodiscard]] Format format();
[[nodiscard]] bool isJson();

// Records a diagnostic. In human mode this only files it for the summary; the
// message itself has already been printed through Log by the caller, which is
// where its formatting and colour live.
void diagnostic(Level level, Diag code, std::string_view message, const Location& location = {});

void progress(std::string_view phase, std::size_t current, std::size_t total, std::string_view item);

void artifact(Artifact entry);

// Emits the closing result event and writes --result, if one was asked for.
// Safe to call when neither is enabled, and safe to call twice: only the first
// call reports.
void finish(std::string_view status, int exitCode);

} // namespace ncp::msg
