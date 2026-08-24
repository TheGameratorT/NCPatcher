#pragma once

// Where the files that ship with ncpatcher live.
//
// `ncp.h`, `ncp_ide.h` and `ncprt.c` are not host headers. They are data files
// force-included into cross-compiled ARM translation units, so /usr/include
// would be the wrong place for them twice over: that directory is for headers
// the *host* compiler consumes, and putting ncp.h there pollutes the host
// include namespace for every other program on the machine. The FHS location
// for arch-independent program data is $datadir/<program>, and because that is
// always reachable relative to the binary, none of this needs XDG lookup, a
// registry key, or a path compiled in at configure time.
//
// This is also what ends the install instructions that said to add the
// *directory* to PATH rather than symlinking the binary: the binary can now be
// anywhere, as long as its ../share/ncpatcher came along or one of the other
// entries below matches.

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ncp::paths {

// Directory the running executable sits in. Queried from the OS once and
// cached; every path below is relative to it.
[[nodiscard]] const std::filesystem::path& exeDir();

// Where data files are looked for, highest priority first:
//
//   1. $NCPATCHER_DATA_DIR      -- dev trees, CI, and images that lay the files
//                                  out somewhere of their own choosing.
//   2. exeDir()/../share/ncpatcher
//                               -- a relocatable prefix. Covers /usr/bin ->
//                                  /usr/share, /usr/local, an AppImage, and any
//                                  --prefix, without the path being baked in.
//   3. exeDir() and exeDir()/include
//                               -- the Windows install directory and today's
//                                  portable release zips, which keep working
//                                  exactly as they did.
//
// Non-existent directories are kept in the list rather than filtered out: when
// nothing is found, the error has to be able to say where it looked.
[[nodiscard]] const std::vector<std::filesystem::path>& dataDirs();

// First existing dataDirs() entry containing `name`, or an empty path.
[[nodiscard]] std::filesystem::path findDataFile(std::string_view name);

// The search list, one indented line each, for a failure message.
[[nodiscard]] std::string dataDirList();

// Locates ncp.h and checks that it is the one this build expects.
//
// A mismatched copy has to be an error rather than a warning. The header is
// where the section attributes and the ncp_* macros are defined, so an old one
// found ahead of the right one does not fail the compile -- it compiles
// cleanly, emits sections the patcher no longer recognises, and produces a ROM
// with the patches quietly missing. That has happened; it cost an afternoon.
//
// Throws ncp::exception naming every directory searched, or both versions.
[[nodiscard]] std::filesystem::path sdkHeader();

} // namespace ncp::paths
