#include "unicode.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace ncp {

namespace {

std::string fromU8(const std::u8string& text)
{
	return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

} // namespace

std::string pathToUtf8(const fs::path& path)
{
	return fromU8(path.u8string());
}

std::string pathToUtf8Generic(const fs::path& path)
{
	return fromU8(path.generic_u8string());
}

fs::path utf8ToPath(std::string_view text)
{
	return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

#ifdef _WIN32

std::wstring toWide(std::string_view utf8)
{
	if (utf8.empty())
		return {};

	const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0);
	if (length <= 0)
		return {};

	std::wstring wide(std::size_t(length), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), wide.data(), length);
	return wide;
}

std::string toUtf8(std::wstring_view wide)
{
	if (wide.empty())
		return {};

	const int length = WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()),
		nullptr, 0, nullptr, nullptr);
	if (length <= 0)
		return {};

	std::string utf8(std::size_t(length), '\0');
	WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()),
		utf8.data(), length, nullptr, nullptr);
	return utf8;
}

#endif

} // namespace ncp
