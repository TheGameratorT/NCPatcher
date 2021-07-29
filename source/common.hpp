#pragma once

#include <stdint.h>
#include <filesystem>
#include <string>
#include <algorithm>
#include <cstring>

#include "oansistream.hpp"

typedef std::int8_t s8;
typedef std::int16_t s16;
typedef std::int32_t s32;
typedef std::int64_t s64;

typedef std::uint8_t u8;
typedef std::uint16_t u16;
typedef std::uint32_t u32;
typedef std::uint64_t u64;

/**
  * @brief Outputs colored text inside colored square brackets
  * 
  * @param c1 Bracket Color
  * @param c2 Text Color
  * @param txt Text
  */
#define OSQRTBRKTS(c1, c2, txt) c1 "[" ANSI_RESET c2 txt ANSI_RESET c1 "]" ANSI_RESET

#define OSTR(x) ANSI_bYELLOW "\"" << x << "\"" ANSI_RESET
#define OREASONNL "\n        "

extern const char* __ncp_OERROR;
extern const char* __ncp_OWARN;
extern const char* __ncp_OINFO;
extern const char* __ncp_OBUILD;
extern const char* __ncp_OLINK;
extern const char* __ncp_OREASON;

#define OERROR __ncp_OERROR
#define OWARN __ncp_OWARN
#define OINFO __ncp_OINFO
#define OBUILD __ncp_OBUILD
#define OLINK __ncp_OLINK
#define OREASON __ncp_OREASON

/*template<typename O, typename I>
std::vector<O> vec_static_cast(const std::vector<I>& in)
{
	size_t count = in.size();
	std::vector<O> out(count);
	for (size_t i = 0; i < count; i++)
		out[i] = in[i];
	return out;
}*/

namespace util
{
	constexpr size_t index_of(const char* val, const char* vals[], size_t size) noexcept
	{
		for (size_t index = 0; index < size; index++)
			if (strcmp(val, vals[index]) == 0)
				return index;
		return -1;
	}

	template <class T1, class T2>
	constexpr size_t index_of(const T1 val, const T2* vals, size_t size) noexcept
	{
		for (size_t index = 0; index < size; index++)
			if (val == vals[index])
				return index;
		return -1;
	}

	static inline std::string str_repl(const std::string& str, char chr, char new_chr)
	{
		std::string new_str = str;
		std::replace(new_str.begin(), new_str.end(), chr, new_chr);
		return new_str;
	}

	int addr_to_int(const std::string& in);
	std::string int_to_addr(int in, int align);
}
