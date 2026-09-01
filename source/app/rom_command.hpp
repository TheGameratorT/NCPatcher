#pragma once

// `ncpatcher rom info | files | extract | pack`.
//
// extract and pack move a ROM between a .nds and a directory: the header, the
// two ARM binaries, the two overlay tables and the overlay files, plus the name
// table, the allocation table, the banner and the NitroFS tree. That set is
// chosen so that what comes out is a directory `rom: dir:` can be pointed at
// and DirRomAccessor can read back in full -- an extraction that stopped at the
// code binaries could only ever be half of a ROM, and every consumer of one had
// to know which half.
//
// `--code-only` asks for just the binaries, which is what this used to write.

#include <filesystem>
#include <ostream>

#include "../rom/dir_accessor.hpp"

namespace ncp::romcmd {

// Both of these report rather than build, so what they print is the product
// and it goes to the `out` they are handed (stdout) while the log goes to
// stderr. A caller piping `rom files --json` into a parser gets the document
// and nothing else; a caller reading the human form still sees the warnings,
// on the stream warnings belong on.

// Prints what the header says. `path` may be a .nds or a directory holding an
// extracted header.
void info(std::ostream& out, const std::filesystem::path& path, const rom::DirLayout& layout);

// Prints the ROM's NitroFS table: every file, its id, its size and its path.
// `json` emits `ncpatcher.files/1` instead, with every entry `unchanged`,
// since this reads a ROM rather than building one and nothing here has
// provenance.
void files(std::ostream& out, const std::filesystem::path& path, const rom::DirLayout& layout, bool json);

struct ExtractOptions
{
	// Just the code binaries, as this used to write. The result is not a ROM a
	// tool can read back on its own, which is why it is not the default.
	bool codeOnly = false;

	// Overlay bytes written decompressed, *and* the compression flag cleared in
	// the emitted overlay table to match.
	//
	// The two go together or not at all. Decompressed bytes under a table still
	// saying "compressed, compressedSize = N" is the one combination that is
	// actively wrong: it is what a naive repack turns into a ROM that hangs. So
	// the flag moves with the bytes, and extraction.json records what the ROM
	// had, which is what lets `pack` put the original form back.
	bool decompressOverlays = false;
};

// Writes `romFile` into `directory`, and an extraction.json describing what it
// wrote. Returns how many files were written.
std::size_t extract(const std::filesystem::path& romFile,
                    const std::filesystem::path& directory,
                    const rom::DirLayout& layout,
                    const ExtractOptions& options = {});

// Reads `directory` back into `romFile`, or into `output` when one is given.
//
// An extraction.json in the directory is read for the form each overlay was
// stored in. That is what makes `--decompress-overlays` safe to pack: an
// overlay the extraction unpacked is compressed again and its table row gets
// its flags back, rather than being handed to the console as raw bytes under a
// row that says otherwise.
std::size_t pack(const std::filesystem::path& romFile,
                 const std::filesystem::path& directory,
                 const std::filesystem::path& output,
                 const rom::DirLayout& layout,
                 u32 arm9Slack);

} // namespace ncp::romcmd
