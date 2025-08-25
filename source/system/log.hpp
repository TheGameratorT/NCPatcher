#pragma once

#include <iostream>
#include <string>
#include <vector>
#include <filesystem>

#include "../utils/types.hpp"

#ifndef LOG_NO_ANSI_MACROS

#define ANSI_RESET "\x1b[0m"

#define ANSI_BLACK "\x1b[30m"
#define ANSI_RED "\x1b[31m"
#define ANSI_GREEN "\x1b[32m"
#define ANSI_YELLOW "\x1b[33m"
#define ANSI_BLUE "\x1b[34m"
#define ANSI_MAGENTA "\x1b[35m"
#define ANSI_CYAN "\x1b[36m"
#define ANSI_WHITE "\x1b[37m"

#define ANSI_bBLACK "\x1b[30;1m"
#define ANSI_bRED "\x1b[31;1m"
#define ANSI_bGREEN "\x1b[32;1m"
#define ANSI_bYELLOW "\x1b[33;1m"
#define ANSI_bBLUE "\x1b[34;1m"
#define ANSI_bMAGENTA "\x1b[35;1m"
#define ANSI_bCYAN "\x1b[36;1m"
#define ANSI_bWHITE "\x1b[37;1m"

#define ANSI_BG_BLACK "\x1b[40m"
#define ANSI_BG_RED "\x1b[41m"
#define ANSI_BG_GREEN "\x1b[42m"
#define ANSI_BG_YELLOW "\x1b[43m"
#define ANSI_BG_BLUE "\x1b[44m"
#define ANSI_BG_MAGENTA "\x1b[45m"
#define ANSI_BG_CYAN "\x1b[46m"
#define ANSI_BG_WHITE "\x1b[47m"

/**
  * @brief Outputs colored text inside colored square brackets
  *
  * @param c1 Bracket Color
  * @param c2 Text Color
  * @param txt Text
  */
#define OSQRTBRKTS(c1, c2, txt) c1 "[" ANSI_RESET c2 txt ANSI_RESET c1 "]" ANSI_RESET

#define OSTR(x) ANSI_bYELLOW "\"" << (x) << "\"" ANSI_RESET
#define OSTRa(x) ANSI_bWHITE "\"" << (x) << "\"" ANSI_RESET
#define OREASONNL "\n        "

extern const char* log_OERROR;
extern const char* log_OWARN;
extern const char* log_OINFO;
extern const char* log_OBUILD;
extern const char* log_OLINK;
extern const char* log_OREASON;

#define OERROR log_OERROR
#define OWARN log_OWARN
#define OINFO log_OINFO
#define OBUILD log_OBUILD
#define OLINK log_OLINK
#define OREASON log_OREASON

#endif

enum class LogMode
{
	Both,
	Console,
	File
};

namespace Log {

enum ColorCode
{
	Black = 30,
	Red = 31,
	Green = 32,
	Yellow = 33,
	Blue = 34,
	Magenta = 35,
	Cyan = 36,
	White = 37,
};

class OutputStream : public std::ostream
{
public:
	OutputStream();
	~OutputStream() override;
};

extern OutputStream out;

void init();
void destroy();

void openLogFile(const std::filesystem::path& path);
void closeLogFile();

void log(const std::string& str);
void info(const std::string& str);
void warn(const std::string& str);
void error(const std::string& str);

void setMode(LogMode mode);

// Gets the cursor position on the console.
Coords getXY();

// Sets the cursor position on the console.
// Warning: This does not apply to the log file, only the console!
void gotoXY(int x, int y);

// Writes a character at the specified position.
// Warning: This does not apply to the log file, only the console!
void writeChar(int x, int y, char chr);

// Writes a character at the specified position with a new color.
// Warning: This does not apply to the log file, only the console!
void writeChar(int x, int y, char chr, int color, bool bold);

// Returns how many lines are left until the end of the console buffer is reached.
std::size_t getRemainingLines();

void showCursor(bool flag);

}
