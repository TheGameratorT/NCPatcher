#pragma once

#include <cstdint>
#include <string>
#include <chrono>
#include <ctime>
#include <iostream>

typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

#define ANSI_RESET         "\033[0m"
#define ANSI_BLACK         "\033[30m"
#define ANSI_RED           "\033[31m"
#define ANSI_GREEN         "\033[32m"
#define ANSI_YELLOW        "\033[33m"
#define ANSI_BLUE          "\033[34m"
#define ANSI_MAGENTA       "\033[35m"
#define ANSI_CYAN          "\033[36m"
#define ANSI_WHITE         "\033[37m"
#define ANSI_BRIGHTBLACK   "\033[30;1m"
#define ANSI_BRIGHTRED     "\033[31;1m"
#define ANSI_BRIGHTGREEN   "\033[32;1m"
#define ANSI_BRIGHTYELLOW  "\033[33;1m"
#define ANSI_BRIGHTBLUE    "\033[34;1m"
#define ANSI_BRIGHTMAGENTA "\033[35;1m"
#define ANSI_BRIGHTCYAN    "\033[36;1m"
#define ANSI_BRIGHTWHITE   "\033[37;1m"

/**
  * @brief Outputs colored text inside colored square brackets
  * 
  * @param c1 Bracket Color
  * @param c2 Text Color
  * @param txt Text
  */
#define OSQRTBRKTS(c1, c2, txt) c1 "[" ANSI_RESET c2 txt ANSI_RESET c1 "]" ANSI_RESET

static const char* OERROR = OSQRTBRKTS(ANSI_BRIGHTWHITE, ANSI_BRIGHTRED, "Error") " ";
static const char* OWARN = OSQRTBRKTS(ANSI_BRIGHTWHITE, ANSI_BRIGHTYELLOW, "Warn") " ";
static const char* OINFO = OSQRTBRKTS(ANSI_BRIGHTWHITE, ANSI_BRIGHTBLUE, "Info") " ";
static const char* OBUILD = OSQRTBRKTS(ANSI_BRIGHTWHITE, ANSI_BRIGHTGREEN, "Build") " ";
static const char* OREASON = "   -->  ";
#define OSTR(x) ANSI_BRIGHTYELLOW "\"" << x << "\"" ANSI_RESET

namespace Util
{
	template <typename TP>
	constexpr std::time_t to_time_t(TP tp)
	{
		using namespace std::chrono;
		auto sctp = time_point_cast<system_clock::duration>(tp - TP::clock::now()
			+ system_clock::now());
		return system_clock::to_time_t(sctp);
	}

	template <class T1, class T2, size_t size>
	constexpr int index_of(const T1 val, const T2(&vals)[size]) noexcept
	{
		for (int index = 0; index < size; index++)
			if (val == vals[index])
				return index;
		return -1;
	}

	static inline
	std::string str_repl(const std::string& str, char chr, char new_chr)
	{
		std::string new_str = str;
		std::replace(new_str.begin(), new_str.end(), chr, new_chr);
		return new_str;
	}
}
