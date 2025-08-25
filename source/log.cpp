#include "log.hpp"

#include <iostream>
#include <fstream>
#include <filesystem>
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

#include "types.hpp"

const char* log_OERROR = OSQRTBRKTS(ANSI_bWHITE, ANSI_bRED, "Error") " ";
const char* log_OWARN = OSQRTBRKTS(ANSI_bWHITE, ANSI_bYELLOW, "Warn") " ";
const char* log_OINFO = OSQRTBRKTS(ANSI_bWHITE, ANSI_bBLUE, "Info") " ";
const char* log_OBUILD = OSQRTBRKTS(ANSI_bWHITE, ANSI_bGREEN, "Build") " ";
const char* log_OLINK = OSQRTBRKTS(ANSI_bWHITE, ANSI_bGREEN, "Link") " ";
const char* log_OREASON = "   -->  ";

namespace Log {

static std::ofstream logFile;
static LogMode logMode = LogMode::Both;
#ifndef _WIN32
static bool xyCapabilityAvailable = true;
#endif

#ifdef _WIN32
static int wincolors[] = {
	0, // Black
	4, // Red
	2, // Green
	6, // Yellow
	1, // Blue
	5, // Magenta
	3, // Cyan
	7  // White
};
#endif

/*
 * This class implements partial support for
 * simple ANSI colored output to the console.
 * */
class OutputStreamBuffer : public std::stringbuf
{
public:
	OutputStreamBuffer()
	{
#ifdef _WIN32
		hOut = GetStdHandle(STD_OUTPUT_HANDLE);
		resetStyles();
#endif
	}

	~OutputStreamBuffer() override
	{
		pubsync();
#ifdef _WIN32
		SetConsoleTextAttribute(hOut, 7);
#endif
	}

	int sync() override
	{
		flushBuffer(str());
		str("");
		return 0; // Always return success
	}

	void flushBuffer(const std::string& buf)
	{
		if (buf.empty())
			return;

#ifndef _WIN32
		if (logMode != LogMode::File && xyCapabilityAvailable)
		{
			std::cout << buf << std::flush;
		}
#endif

		auto isEndChar = [](char c){ return (c < '0' || c > '9') && c != ';'; };

		SizeT bufl = buf.length();
		SizeT cpos = 0;
		SizeT lpos = 0;

		std::string_view bufView(buf);

		// Process ANSI escape sequences
		while ((cpos = buf.find('\x1b', cpos)) != std::string::npos)
		{
			// Prevent infinite loops with malformed escape sequences
			if (cpos + 1 >= bufl || buf[cpos + 1] != '[')
			{
				cpos++;
				continue;
			}

			// Print the section between the previous ANSI code and the newly found one
			outputBuffer(bufView.substr(lpos, cpos - lpos));

			cpos += 2; // Skip the \x1b and [

			char op = '\0';

			// Find the ANSI operator code
			std::size_t ccpos = cpos;
			while (ccpos < bufl)
			{
				char c = buf[ccpos];
				if (isEndChar(c))
				{
					op = buf[ccpos];
					break;
				}
				ccpos++;
			}

			SizeT lapos = cpos; // Set the position for the last argument

			while (cpos < bufl)
			{
				char c = buf[cpos];
				bool reachedEnd = isEndChar(c);
				if (c == ';' || reachedEnd)
				{
					SizeT argLen = cpos - lapos;
#ifdef _WIN32
					if (op == 'm' && argLen < 10) // Safety check for argument length
					{
						int arg = argLen == 0 ? 0 : std::stoi(std::string(bufView.substr(lapos, argLen)));
						applyCode(arg);
					}
#endif
					lapos = cpos + 1;
				}
				cpos++;
				if (reachedEnd)
					break;
			}
			lpos = cpos;
		}

		outputBuffer(bufView.substr(lpos)); // Print the remaining text
	}

	static void outputBuffer(const std::string_view& str)
	{
		if (str.empty())
			return;
			
#ifdef _WIN32
		if (logMode != LogMode::File)
			std::cout << str << std::flush;
#endif
		if (logMode != LogMode::Console)
		{
#ifndef _WIN32
			if (!xyCapabilityAvailable)
				std::cout << str << std::flush;
#endif
			if (logFile.is_open())
				logFile << str << std::flush;
		}
	}

#ifdef _WIN32
	void applyCode(int value)
	{
		if (value == 0) // Reset
		{
			resetStyles();
		}
		else if (value == 1) // Bold
		{
			if (!boldEnabled)
			{
				int fgAttr = txtAttr & 0xF;
				txtAttr &= ~0xF;
				txtAttr |= fgAttr + 8;
				SetConsoleTextAttribute(hOut, txtAttr);
			}
			boldEnabled = true;
		}
		else if (value >= 30) // Text or Background
		{
			if (value < 40) // Text
			{
				txtAttr &= ~0xF;
				txtAttr |= wincolors[value - 30] + (int(boldEnabled) * 8);
			}
			else // Background
			{
				txtAttr &= ~0xF0;
				txtAttr |= wincolors[value - 40];
			}
			SetConsoleTextAttribute(hOut, txtAttr);
		}
	}

	void resetStyles()
	{
		txtAttr = 7;
		boldEnabled = false;
		SetConsoleTextAttribute(hOut, 7);
	}

private:
	HANDLE hOut;
	WORD txtAttr;
	bool boldEnabled;
#endif
};

OutputStream::OutputStream() :
	std::ostream(new OutputStreamBuffer())
{}

OutputStream::~OutputStream()
{
	delete rdbuf();
}

OutputStream out;

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
}

void destroy()
{
	closeLogFile();
}

void openLogFile(const std::filesystem::path& path)
{
	logFile.open(path);
	if (!logFile.is_open())
		throw std::runtime_error("Could not open output log file!");
}

void closeLogFile()
{
	if (logFile.is_open())
		logFile.close();
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
			attr |= wincolors[color - 30] + (int(bold) * 8);
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
