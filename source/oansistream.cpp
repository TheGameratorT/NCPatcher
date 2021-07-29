#ifndef _WIN32

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
	void applyCode(long val);

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
	size_t cpos = 0;
	size_t lpos = 0;
	while ((cpos = buf.find('\x1b', cpos)) != std::string::npos)
	{
		std::cout << std::string_view(buf).substr(lpos, cpos - lpos) << std::flush;
		cpos += 2;
		while (cpos < bufl)
		{
			char c = buf[cpos++];
			if ((c < '0' || c > '9') && c != ';')
				break;
		}
		lpos = cpos;
	}
	std::cout << &buf[lpos] << std::flush;
}

void oansistreambuf::applyCode(long val)
{
	if (val == 0) {
		txtAttr = 7;
		SetConsoleTextAttribute(hOut, 7);
	}
	else if (val >= 30) {
		if (val < 40) {
			txtAttr &= ~0xF;
			txtAttr |= wincolors[val - 30];
		}
		else {
			txtAttr &= ~0xF0;
			txtAttr |= wincolors[val - 40];
		}
		SetConsoleTextAttribute(hOut, txtAttr);
	}
}

#endif
