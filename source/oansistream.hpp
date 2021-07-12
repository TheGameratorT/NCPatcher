#pragma once

#ifndef __OCOLORSTREAM_NO_ANSI_MACROS

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

#define ANSI_BG_bBLACK "\x1b[40;1m"
#define ANSI_BG_bRED "\x1b[41;1m"
#define ANSI_BG_bGREEN "\x1b[42;1m"
#define ANSI_BG_bYELLOW "\x1b[43;1m"
#define ANSI_BG_bBLUE "\x1b[44;1m"
#define ANSI_BG_bMAGENTA "\x1b[45;1m"
#define ANSI_BG_bCYAN "\x1b[46;1m"
#define ANSI_BG_bWHITE "\x1b[47;1m"

#endif

#ifdef _WIN32

#include <ostream>
#include <sstream>

class oansistream : public std::ostream
{
public:
    oansistream();
    ~oansistream();
};

namespace ansi {
    extern oansistream cout;
}

#else

#include <iostream>

namespace ansi {
    constexpr inline std::ostream& cout = std::cout;
}

#endif
