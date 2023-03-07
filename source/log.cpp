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
#include <unistd.h>
#include <termios.h>
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
		return std::cout ? 0 : -1;
	}

	void flushBuffer(const std::string& buf)
	{
#ifndef _WIN32
		if (logMode != LogMode::File)
			std::cout << buf << std::flush;
#endif

		auto isEndChar = [](char c){ return (c < '0' || c > '9') && c != ';'; };

		SizeT bufl = buf.length();
		SizeT cpos = 0;
		SizeT lpos = 0;

		std::string_view bufView(buf);

		while ((cpos = buf.find('\x1b', cpos)) != std::string::npos)
		{
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
					if (op == 'm')
					{
						int arg = argLen == 0 ? 0 : std::stoi(std::string(bufView.substr(lapos, argLen)));
						applyCode(arg);
					}
#endif
					lapos = cpos + 1;
					/*if (argLen != 0) // argLen can only be 0, when the end has been reached so no need to break
					{
					}*/
				}
				cpos++;
				if (reachedEnd)
					break;
			}
			lpos = cpos;
		}

		outputBuffer(&buf[lpos]); // Print the remaining text
	}

	static void outputBuffer(const std::string_view& str)
	{
#ifdef _WIN32
		if (logMode != LogMode::File)
			std::cout << str << std::flush;
#endif
		if (logMode != LogMode::Console)
		{
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
	if (hStdOut)
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
	if (hStdOut)
	{
		COORD coord = {static_cast<SHORT>(x), static_cast<SHORT>(y)};
		SetConsoleCursorPosition(hStdOut, coord);
	}
}

void showCursor(bool flag)
{
	HANDLE hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
	if (hStdOut)
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

	struct termios term, restore;

	tcgetattr(0, &term);
	tcgetattr(0, &restore);
	term.c_lflag &= ~(ICANON|ECHO);
	tcsetattr(0, TCSANOW, &term);

	write(1, "\033[6n", 4);

	for(i = 0, ch = 0; ch != 'R'; i++)
	{
		ret = read(0, &ch, 1);
		if (!ret)
		{
			tcsetattr(0, TCSANOW, &restore);
			fprintf(stderr, "getpos: error reading response!\n");
			return coords;
		}
		buf[i] = ch;
	}

	if (i < 2)
	{
		tcsetattr(0, TCSANOW, &restore);
		printf("Log::getXY() error: i < 2\n");
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
	std::cout << "\x1b[" << (y + 1) << ";" << (x + 1) << "H" << std::flush;
}

void showCursor(bool flag)
{
	std::cout << (flag ? "\x1b[?25h" : "\x1b[?25l") << std::flush;
}

#endif

}
