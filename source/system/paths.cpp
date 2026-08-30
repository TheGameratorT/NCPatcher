#include "paths.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include "log.hpp"
#include "except.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include <windows.h>
#elif __linux__
#include <unistd.h>
#elif __APPLE__
#include <mach-o/dyld.h>
#include <limits.h>
#else
#error Unsupported operating system
#endif

// Where the install rules put the data files, expressed relative to the
// directory holding the binary. CMake computes it from CMAKE_INSTALL_BINDIR and
// CMAKE_INSTALL_DATADIR so that a prefix using bin64/ or an unusual datadir
// still resolves; the default is what a stock GNU layout gives.
#ifndef NCP_DATA_RELDIR
#define NCP_DATA_RELDIR "../share/ncpatcher"
#endif

// Bumped whenever sdk/ncp.h changes in a way the patcher cares about. CMake
// checks at configure time that the header carries this same number, so the two
// cannot drift apart in the source tree, only on a machine where an old copy
// is still installed, which is exactly the case this exists to catch.
#ifndef NCP_SDK_VERSION
#define NCP_SDK_VERSION 1
#endif

namespace fs = std::filesystem;

namespace ncp::paths {

namespace {

fs::path queryExeDir()
{
#ifdef _WIN32

	// The Windows path limit is 0x7FFF characters; anything past that is not a
	// path we can hold, so the doubling stops there rather than looping.
	DWORD length = 0x200;
	std::vector<wchar_t> filename;

	while (true)
	{
		filename.resize(length);
		const DWORD written = GetModuleFileNameW(nullptr, filename.data(), length);
		if (written == 0)
			throw ncp::exception("Could not query the application directory path.");
		if (written < length)
			return fs::path(std::wstring(filename.data(), written)).parent_path();

		if (length >= 0x8000)
			throw ncp::exception("Could not query the application directory path: path too long.");
		length *= 2;
	}

#elif __linux__

	std::size_t length = 0x200;
	std::vector<char> filename;

	while (true)
	{
		filename.resize(length);
		const ssize_t readLength = readlink("/proc/self/exe", filename.data(), filename.size());

		if (readLength == -1)
			throw ncp::exception("Could not query the application directory path: cannot read /proc/self/exe.");

		if (static_cast<std::size_t>(readLength) < length)
			return fs::path(std::string(filename.data(), static_cast<std::size_t>(readLength))).parent_path();

		if (length >= 0x10000)
			throw ncp::exception("Could not query the application directory path: path too long.");
		length *= 2;
	}

#elif __APPLE__

	char buffer[PATH_MAX];
	uint32_t size = PATH_MAX;
	if (_NSGetExecutablePath(buffer, &size) != 0)
		throw ncp::exception("Could not query the application directory path.");

	// The returned path may contain symlinks and .. components; canonical is
	// what makes ../share resolve to the same place a package installed it.
	std::error_code error;
	const fs::path canonical = fs::weakly_canonical(fs::path(buffer), error);
	return (error ? fs::path(buffer) : canonical).parent_path();

#endif
}

std::vector<fs::path> buildDataDirs()
{
	std::vector<fs::path> dirs;

	if (const char* override_ = std::getenv("NCPATCHER_DATA_DIR"); override_ && *override_)
		dirs.emplace_back(override_);

	const fs::path& exe = exeDir();

	// lexically_normal so the failure message reads "/usr/share/ncpatcher"
	// rather than "/usr/bin/../share/ncpatcher".
	dirs.push_back((exe / NCP_DATA_RELDIR).lexically_normal());
	dirs.push_back(exe);
	dirs.push_back(exe / "include");

	// The portable layout makes two of these the same directory, and a failure
	// message that lists one place twice reads like a bug in the search.
	for (auto it = dirs.begin(); it != dirs.end(); )
		it = std::find(dirs.begin(), it, *it) != it ? dirs.erase(it) : it + 1;

	return dirs;
}

// Reads `#define __ncp_sdk_version N` out of a copy of ncp.h. Returns -1
// when the header does not carry a stamp at all, which is what every release
// before this one looks like.
int readHeaderVersion(const fs::path& header)
{
	std::ifstream file(header);
	if (!file.is_open())
		throw ncp::file_error(header, ncp::file_error::read);

	// The stamp is deliberately in the first few lines, ahead of the language
	// guard, so that finding it never means parsing the whole header.
	std::string line;
	for (int lineNumber = 0; lineNumber < 64 && std::getline(file, line); lineNumber++)
	{
		const std::size_t at = line.find("__ncp_sdk_version");
		if (at == std::string::npos || line.find("#define") == std::string::npos)
			continue;

		std::istringstream rest(line.substr(at + std::strlen("__ncp_sdk_version")));
		int version = -1;
		rest >> version;
		return rest.fail() ? -1 : version;
	}

	return -1;
}

} // namespace

const fs::path& exeDir()
{
	static const fs::path dir = queryExeDir();
	return dir;
}

const std::vector<fs::path>& dataDirs()
{
	static const std::vector<fs::path> dirs = buildDataDirs();
	return dirs;
}

fs::path findDataFile(std::string_view name)
{
	for (const fs::path& dir : dataDirs())
	{
		const fs::path candidate = dir / name;
		std::error_code error;
		if (fs::is_regular_file(candidate, error))
			return candidate;
	}
	return {};
}

std::string dataDirList()
{
	std::ostringstream oss;
	for (const fs::path& dir : dataDirs())
		oss << OREASONNL "  " << dir.string();
	return oss.str();
}

fs::path sdkHeader()
{
	const fs::path header = findDataFile("ncp.h");
	if (header.empty())
	{
		std::ostringstream oss;
		oss << "Could not find " << OSTR("ncp.h") << ", which every source file is compiled against."
			<< OREASONNL << "Searched:" << dataDirList()
			<< OREASONNL << "If ncpatcher is installed, its data files were not; set "
			<< OSTRa("NCPATCHER_DATA_DIR") << " to point at them.";
		throw ncp::exception(oss.str());
	}

	const int version = readHeaderVersion(header);
	if (version != NCP_SDK_VERSION)
	{
		std::ostringstream oss;
		oss << "The SDK header " << OSTR(header.string()) << " is from a different version of NCPatcher."
			<< OREASONNL << "It declares SDK version "
			<< (version < 0 ? std::string("none") : std::to_string(version))
			<< ", and this build needs " << NCP_SDK_VERSION << "."
			<< OREASONNL << "Building against it would compile, and would then leave patches out of the ROM"
			<< OREASONNL << "without saying so, which is why this is an error."
			<< OREASONNL << "Update the installed data files, or point " << OSTRa("NCPATCHER_DATA_DIR")
			<< " at the matching ones.";
		throw ncp::exception(oss.str());
	}

	return header;
}

} // namespace ncp::paths
