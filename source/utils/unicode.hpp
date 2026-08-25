#pragma once

// Turning a path into a string that is going to be handed to another program.
//
// On Linux and macOS this is nothing: a path is bytes, the bytes are UTF-8, and
// `path::string()` hands them back unchanged. Windows is where it matters. There
// a path is UTF-16, and `path::string()` encodes it down to the machine's ANSI
// code page -- which under MSVC cannot represent a user called José, so the
// characters are lost before any of this code sees them. (libstdc++ happens to
// use UTF-8 for the same call, so a MinGW build has always been fine, which is
// exactly why the problem is easy to miss.)
//
// So: paths become strings through pathToUtf8, the whole program's narrow
// strings are UTF-8, and the conversion to UTF-16 happens once, in Process,
// where a string is handed to Windows.
//
// This is not for paths used to *open* files. Those stay std::filesystem::path,
// whose native form is already wide, and never go through here at all.

#include <filesystem>
#include <string>
#include <string_view>

namespace ncp {

[[nodiscard]] std::string pathToUtf8(const std::filesystem::path& path);
[[nodiscard]] std::string pathToUtf8Generic(const std::filesystem::path& path);
[[nodiscard]] std::filesystem::path utf8ToPath(std::string_view text);

#ifdef _WIN32
[[nodiscard]] std::wstring toWide(std::string_view utf8);
[[nodiscard]] std::string toUtf8(std::wstring_view wide);
#endif

} // namespace ncp
