#pragma once

#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <array>

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

template<class... Args>
constexpr std::string concat(std::size_t preAllocSz, const Args&... args)
{
	std::string result;
	result.reserve(preAllocSz);
	for (auto s : {std::string_view(args)...})
		result += s;
	return result;
}

int addrToInt(const std::string& in);
std::string intToAddr(int in, int align);

}
