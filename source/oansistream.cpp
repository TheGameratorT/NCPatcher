#ifdef _WIN32

#include "oansistream.hpp"

#include <windows.h>
#include <iostream>

static int wincolors[] = {
	0, //black
	4, //red
	2, //green
	6, //yellow
	1, //blue
	5, //magenta
	3, //cyan
	7  //white
};

namespace ansi {
	oansistream cout;
}

class oansistreambuf : public std::stringbuf
{
public:
	oansistreambuf();
	~oansistreambuf();

	int sync() override;

	void flushBuffer(const std::string& buf);
	void applyCode(long val, long arg);

private:
	HANDLE hOut;
	int txtAttr;
};

oansistream::oansistream() :
	std::ostream(new oansistreambuf())
{}

oansistream::~oansistream() {
	delete rdbuf();
}

oansistreambuf::oansistreambuf()
{
	hOut = GetStdHandle(STD_OUTPUT_HANDLE);
	SetConsoleTextAttribute(hOut, 7);
	txtAttr = 7;
}

oansistreambuf::~oansistreambuf()
{
	pubsync();
	SetConsoleTextAttribute(hOut, 7);
}

int oansistreambuf::sync()
{
	flushBuffer(str());
	str("");
	return std::cout ? 0 : -1;
}

void oansistreambuf::flushBuffer(const std::string& buf)
{
	size_t bufl = buf.length();
	size_t ppos = 0; // previous pos
	size_t pos = 0; // current pos
	while ((pos = buf.find('\x1b', pos)) != std::string::npos)
	{
		std::cout << buf.substr(ppos, pos - ppos) << std::flush;

		// parse val

		size_t valoff = pos + 2;
		if (valoff >= bufl) {
			break;
		}

		size_t vall;
		long val = std::stoul(&buf[valoff], &vall, 10);

		// parse arg

		size_t next = valoff + vall;

		size_t argoff = next + 1;
		size_t argl = 0;
		long arg = 0;

		if (argoff < bufl) {
			if (buf[argoff - 1] == ';') {
				arg = std::stoul(&buf[argoff], &argl, 10);
				next = argoff + argl;
			}
		}

		applyCode(val, arg);

		pos = next + 1;
		ppos = pos;
	}
	std::cout << &buf[ppos] << std::flush;
}

void oansistreambuf::applyCode(long val, long arg)
{
	if (val == 0) {
		txtAttr = 7;
		SetConsoleTextAttribute(hOut, 7);
	}
	else if (val >= 30) {
		int bright = arg * 8;
		if (val < 40) {
			txtAttr &= ~0xF;
			txtAttr |= wincolors[val - 30] + bright;
		}
		else {
			txtAttr &= ~0xF0;
			txtAttr |= wincolors[val - 40] + bright;
		}
		SetConsoleTextAttribute(hOut, txtAttr);
	}
}

#endif
