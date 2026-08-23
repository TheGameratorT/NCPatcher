#include "log.hpp"

#include <iostream>
#include <filesystem>
#include <memory>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include <windows.h>
#else
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#endif

#include "../utils/types.hpp"
#include "log_sink.hpp"

const char* log_OERROR = OSQRTBRKTS(ANSI_bWHITE, ANSI_bRED, "Error") " ";
const char* log_OWARN = OSQRTBRKTS(ANSI_bWHITE, ANSI_bYELLOW, "Warn") " ";
const char* log_OINFO = OSQRTBRKTS(ANSI_bWHITE, ANSI_bBLUE, "Info") " ";
const char* log_OBUILD = OSQRTBRKTS(ANSI_bWHITE, ANSI_bGREEN, "Build") " ";
const char* log_OLINK = OSQRTBRKTS(ANSI_bWHITE, ANSI_bGREEN, "Link") " ";
const char* log_OREASON = "   -->  ";

namespace Log {

static LogMode logMode = LogMode::Both;
#ifndef _WIN32
static bool xyCapabilityAvailable = true;
#endif

// Collects everything streamed into Log::out until a flush, then hands the
// whole chunk to the sinks.
//
// It deliberately knows nothing about ANSI, files or consoles: a message is
// rendered once, here, and each sink decides what to do with it. Buffering to a
// flush is what keeps a styled line whole -- a sink that has to translate
// escapes into console attributes cannot do so if the escape and the text it
// applies to arrive in separate calls.
class OutputStreamBuffer : public std::stringbuf
{
public:
	~OutputStreamBuffer() override
	{
		pubsync();
	}

	int sync() override
	{
		dispatch(str());
		str("");
		return 0;
	}
};

OutputStream::OutputStream() :
	std::ostream(new OutputStreamBuffer())
{}

OutputStream::~OutputStream()
{
	delete rdbuf();
}

OutputStream out;

// Which console sink applies is fixed for the run: a styling sink when the
// terminal can render escapes and move the cursor, a plain one otherwise.
static void installConsoleSink()
{
	if (hasSink(SinkKind::Terminal) || hasSink(SinkKind::Plain))
		return;

	if (terminalSupportsCursor())
		addSink(std::make_unique<TerminalSink>());
	else
		addSink(std::make_unique<PlainSink>());
}

void init()
{
	std::ios_base::sync_with_stdio(false);

#ifndef _WIN32
	// Test XY capability by attempting to query cursor position with timeout
	struct termios term, restore;
	tcgetattr(0, &term);
	tcgetattr(0, &restore);
	term.c_lflag &= ~(ICANON|ECHO);
	tcsetattr(0, TCSANOW, &term);

	int ret = write(1, "\033[6n", 4);
	if (ret == -1)
	{
		xyCapabilityAvailable = false;
		tcsetattr(0, TCSANOW, &restore);
		installConsoleSink();
		return;
	}

	// Add timeout to prevent indefinite blocking during capability check
	fd_set read_fds;
	struct timeval timeout;
	FD_ZERO(&read_fds);
	FD_SET(0, &read_fds);
	timeout.tv_sec = 1;  // 1 second timeout
	timeout.tv_usec = 0;

	int select_ret = select(1, &read_fds, NULL, NULL, &timeout);
	if (select_ret <= 0)
	{
		xyCapabilityAvailable = false;
		tcsetattr(0, TCSANOW, &restore);
		installConsoleSink();
		return;
	}

	// Read and discard the response to complete the capability check
	char buf[30];
	char ch = 0;
	int i = 0;
	while (ch != 'R' && i < 29)
	{
		ret = read(0, &ch, 1);
		if (!ret)
		{
			xyCapabilityAvailable = false;
			break;
		}
		buf[i] = ch;
		i++;
	}

	tcsetattr(0, TCSANOW, &restore);
#endif

	installConsoleSink();
}

void destroy()
{
	closeLogFile();
}

void openLogFile(const std::filesystem::path& path)
{
	removeSinks(SinkKind::File);
	addSink(std::make_unique<FileSink>(path));
}

void closeLogFile()
{
	removeSinks(SinkKind::File);
}

void log(const std::string& str)
{
	out << str << std::endl;
}

void info(const std::string& str)
{
	out << OINFO << str << std::endl;
}

void warn(const std::string& str)
{
	out << OWARN << str << std::endl;
}

void error(const std::string& str)
{
	out << OERROR << str << std::endl;
}

void setMode(LogMode mode)
{
	logMode = mode;
}

LogMode getMode()
{
	return logMode;
}

bool terminalSupportsCursor()
{
#ifdef _WIN32
	return true;
#else
	return xyCapabilityAvailable;
#endif
}

#ifdef _WIN32

Coords getXY()
{
	Coords coords{0, 0};
	HANDLE hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
	if (hStdOut != INVALID_HANDLE_VALUE)
	{
		CONSOLE_SCREEN_BUFFER_INFO cbsi;
		if (GetConsoleScreenBufferInfo(hStdOut, &cbsi))
		{
			COORD& coord = cbsi.dwCursorPosition;
			coords = { coord.X, coord.Y };
		}
	}
	return coords;
}

void gotoXY(int x, int y)
{
	HANDLE hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
	if (hStdOut != INVALID_HANDLE_VALUE)
	{
		COORD coord{SHORT(x), SHORT(y)};
		SetConsoleCursorPosition(hStdOut, coord);
	}
}

void writeChar(int x, int y, char chr)
{
	HANDLE hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
	if (hStdOut != INVALID_HANDLE_VALUE)
	{
		DWORD dw;
		COORD coord{SHORT(x), SHORT(y)};
		
		WriteConsoleOutputCharacterA(hStdOut, &chr, 1, coord, &dw);
	}
}

void writeChar(int x, int y, char chr, int color, bool bold)
{
	HANDLE hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
	if (hStdOut != INVALID_HANDLE_VALUE)
	{
		DWORD dw;
		COORD coord{SHORT(x), SHORT(y)};

		WORD attr;
		if (ReadConsoleOutputAttribute(hStdOut, &attr, 1, coord, &dw))
		{
			attr &= ~0xF;
			attr |= ansiColorToConsole(color) + (int(bold) * 8);
			WriteConsoleOutputAttribute(hStdOut, &attr, 1, coord, &dw);
		}

		WriteConsoleOutputCharacterA(hStdOut, &chr, 1, coord, &dw);
	}
}

std::size_t getRemainingLines()
{
	HANDLE hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
	if (hStdOut != INVALID_HANDLE_VALUE)
	{
		CONSOLE_SCREEN_BUFFER_INFO cbsi;
		if (GetConsoleScreenBufferInfo(hStdOut, &cbsi))
		{
			COORD& bufSize = cbsi.dwSize;
			COORD& cursorPos = cbsi.dwCursorPosition;
			return bufSize.Y - cursorPos.Y - 1;
		}
	}
	return 0;
}

void showCursor(bool flag)
{
	HANDLE hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
	if (hStdOut != INVALID_HANDLE_VALUE)
	{
		CONSOLE_CURSOR_INFO cursorInfo;
		if (GetConsoleCursorInfo(hStdOut, &cursorInfo))
		{
			cursorInfo.bVisible = flag;
			SetConsoleCursorInfo(hStdOut, &cursorInfo);
		}
	}
}

#else

Coords getXY()
{
	char buf[30];
	int ret, i, pow;
	char ch;
	Coords coords{0,0};

	if (!xyCapabilityAvailable)
	{
		return coords;
	}

	struct termios term, restore;

	tcgetattr(0, &term);
	tcgetattr(0, &restore);
	term.c_lflag &= ~(ICANON|ECHO);
	tcsetattr(0, TCSANOW, &term);

	ret = write(1, "\033[6n", 4);
	if (ret == -1)
	{
		tcsetattr(0, TCSANOW, &restore);
		fprintf(stderr, "Log::getXY() error: query failed!\n");
		return coords;
	}

	for(i = 0, ch = 0; ch != 'R' && i < 29; i++)
	{
		ret = read(0, &ch, 1);
		if (!ret)
		{
			tcsetattr(0, TCSANOW, &restore);
			fprintf(stderr, "Log::getXY() error: response read failed!\n");
			return coords;
		}
		buf[i] = ch;
	}

	if (i < 2)
	{
		tcsetattr(0, TCSANOW, &restore);
		fprintf(stderr, "Log::getXY() error: i < 2\n");
		return coords;
	}

	for(i -= 2, pow = 1; buf[i] != ';'; i--, pow *= 10)
		coords.x += (buf[i] - '0') * pow;

	for(i--, pow = 1; buf[i] != '['; i--, pow *= 10)
		coords.y += (buf[i] - '0') * pow;

	coords.x -= 1;
	coords.y -= 1;

	tcsetattr(0, TCSANOW, &restore);
	return coords;
}

void gotoXY(int x, int y)
{
	if (!xyCapabilityAvailable)
		return;
		
	if (x < 0 || y < 0)
	{
		struct winsize ws;
		ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
		x = 0;
		y = ws.ws_row-1;
	}
	std::cout << "\x1b[" << (y + 1) << ';' << (x + 1) << 'H' << std::flush;
}

void writeChar(int x, int y, char chr)
{
	if (!xyCapabilityAvailable)
		return;
		
	Coords coords = getXY();
	gotoXY(x, y);
	std::cout << chr << std::flush;
	gotoXY(coords.x, coords.y);
}

void writeChar(int x, int y, char chr, int color, bool bold)
{
	if (!xyCapabilityAvailable)
		return;
		
	Coords coords = getXY();
	gotoXY(x, y);
	std::cout << "\x1b[" << std::to_string(color);
	if (bold)
		std::cout << ";1";
	std::cout << 'm' << chr << ANSI_RESET << std::flush;
	gotoXY(coords.x, coords.y);
}

std::size_t getRemainingLines()
{
	if (!xyCapabilityAvailable)
		return 0;
		
	struct winsize ws;
	ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
	Coords cursorPos = getXY();
	return ws.ws_row - cursorPos.y - 1;
}

void showCursor(bool flag)
{
	if (!xyCapabilityAvailable)
		return;
		
	std::cout << (flag ? "\x1b[?25h" : "\x1b[?25l") << std::flush;
}

#endif

}
