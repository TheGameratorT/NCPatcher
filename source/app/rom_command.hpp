#pragma once

// `ncpatcher rom info | files | extract | pack`.
//
// extract and pack move the *code* binaries between a .nds and a directory --
// the header, the two ARM binaries, the two overlay tables and the overlay
// files. That is exactly the set the patcher works on, and exactly the set
// every project currently extracts with a script of its own. Pulling the whole
// NitroFS apart is a different job, and ndstool already does it.

#include <filesystem>

#include "../rom/dir_accessor.hpp"

namespace ncp::romcmd {

// Prints what the header says. `path` may be a .nds or a directory holding an
// extracted header.
void info(const std::filesystem::path& path, const rom::DirLayout& layout);

// Prints the ROM's NitroFS table: every file, its id, its size and its path.
// `json` emits `ncpatcher.files/1` instead, with every entry `unchanged` --
// this reads a ROM rather than building one, so nothing here has provenance.
void files(const std::filesystem::path& path, const rom::DirLayout& layout, bool json);

// Writes the code binaries of `romFile` into `directory`.
// Returns how many files were written.
std::size_t extract(const std::filesystem::path& romFile,
                    const std::filesystem::path& directory,
                    const rom::DirLayout& layout);

// Reads the code binaries out of `directory` and writes them into `romFile`,
// or into `output` when one is given.
std::size_t pack(const std::filesystem::path& romFile,
                 const std::filesystem::path& directory,
                 const std::filesystem::path& output,
                 const rom::DirLayout& layout,
                 u32 arm9Slack);

} // namespace ncp::romcmd
