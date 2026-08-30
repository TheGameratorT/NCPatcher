#pragma once

#include <cstring>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>

namespace Util {

constexpr size_t indexOf(const char* val, const char* vals[], size_t size) noexcept
{
	for (size_t index = 0; index < size; index++)
		if (strcmp(val, vals[index]) == 0)
			return index;
	return -1;
}

template <class T1, class T2>
constexpr size_t indexOf(const T1 val, const T2* vals, size_t size) noexcept
{
	for (size_t index = 0; index < size; index++)
		if (val == vals[index])
			return index;
	return -1;
}

static inline std::string strRepl(std::string str, char chr, char new_chr)
{
	std::replace(str.begin(), str.end(), chr, new_chr);
	return str;
}

template <typename TP>
constexpr std::time_t toTimeT(TP tp)
{
	using namespace std::chrono;
	auto sctp = time_point_cast<system_clock::duration>(tp - TP::clock::now() + system_clock::now());
	return system_clock::to_time_t(sctp);
}

template <typename T>
constexpr bool overlaps(T x1, T x2, T y1, T y2)
{
	return x2 > y1 && y2 > x1;
}

int addrToInt(const std::string& in);
std::string intToAddr(int in, int align, bool prefix = true);

void printDataAsHex(const void* data, std::size_t size, std::size_t rowlen);

// Shortens `path` for display and for command lines when it sits under `base`.
// Anything outside `base` comes back untouched: a long "../.." chain is neither
// shorter nor clearer than the absolute path it would replace.
std::filesystem::path relativeIfSubpath(const std::filesystem::path& path, const std::filesystem::path& base);

}
