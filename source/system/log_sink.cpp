#include "log_sink.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include <windows.h>
#endif

#include "ansi.hpp"

namespace Log {

namespace {

std::vector<std::unique_ptr<Sink>> s_sinks;

} // namespace

#ifdef _WIN32

int ansiColorToConsole(int ansiCode)
{
	static const int winColors[] = {
		0, // Black
		4, // Red
		2, // Green
		6, // Yellow
		1, // Blue
		5, // Magenta
		3, // Cyan
		7  // White
	};

	const int index = (ansiCode >= 40 ? ansiCode - 40 : ansiCode - 30);
	if (index < 0 || index > 7)
		return 7;
	return winColors[index];
}

#endif

// TerminalSink ==========================================================

#ifdef _WIN32

namespace {

HANDLE s_conOut = nullptr;
WORD s_txtAttr = 7;
bool s_boldEnabled = false;

void resetStyles()
{
	s_txtAttr = 7;
	s_boldEnabled = false;
	SetConsoleTextAttribute(s_conOut, 7);
}

void applyCode(int value)
{
	if (value < 0) // malformed parameter
		return;

	if (value == 0) // reset
	{
		resetStyles();
	}
	else if (value == 1) // bold
	{
		// Bold is the high-intensity bit of the foreground colour, so applying
		// it twice would push the colour out of range.
		if (!s_boldEnabled)
		{
			int fgAttr = s_txtAttr & 0xF;
			s_txtAttr &= ~0xF;
			s_txtAttr |= fgAttr + 8;
			SetConsoleTextAttribute(s_conOut, s_txtAttr);
		}
		s_boldEnabled = true;
	}
	else if (value >= 30 && value < 48) // foreground or background colour
	{
		if (value < 40)
		{
			s_txtAttr &= ~0xF;
			s_txtAttr |= ansiColorToConsole(value) + (int(s_boldEnabled) * 8);
		}
		else
		{
			s_txtAttr &= ~0xF0;
			s_txtAttr |= ansiColorToConsole(value) << 4;
		}
		SetConsoleTextAttribute(s_conOut, s_txtAttr);
	}
}

} // namespace

TerminalSink::TerminalSink(bool useStderr) :
	m_stream(useStderr ? &std::cerr : &std::cout)
{
	s_conOut = GetStdHandle(useStderr ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
	resetStyles();
}

TerminalSink::~TerminalSink()
{
	SetConsoleTextAttribute(s_conOut, 7);
}

void TerminalSink::write(std::string_view text)
{
	// The console does not interpret escapes, so they are applied as attribute
	// changes between the literal runs they were sitting between.
	std::ostream& out = *m_stream;
	Ansi::parse(text,
		[&out](std::string_view run) { out << run << std::flush; },
		[](char finalByte, const std::vector<int>& params)
		{
			if (finalByte != 'm')
				return;
			for (int p : params)
				applyCode(p);
		});
}

#else

TerminalSink::TerminalSink(bool useStderr) :
	m_stream(useStderr ? &std::cerr : &std::cout)
{}

TerminalSink::~TerminalSink() = default;

void TerminalSink::write(std::string_view text)
{
	// The terminal renders the escapes itself, so nothing needs decoding.
	*m_stream << text << std::flush;
}

#endif

// PlainSink =============================================================

PlainSink::PlainSink(bool useStderr) :
	m_stream(useStderr ? &std::cerr : &std::cout)
{}

void PlainSink::write(std::string_view text)
{
	*m_stream << Ansi::strip(text) << std::flush;
}

// FileSink ==============================================================

struct FileSink::Impl
{
	std::ofstream file;
};

FileSink::FileSink(const std::filesystem::path& path)
	: m_impl(std::make_unique<Impl>())
{
	m_impl->file.open(path);
	if (!m_impl->file.is_open())
		throw std::runtime_error("Could not open output log file!");
}

FileSink::~FileSink()
{
	if (m_impl->file.is_open())
		m_impl->file.close();
}

void FileSink::write(std::string_view text)
{
	m_impl->file << Ansi::strip(text) << std::flush;
}

// Registry ==============================================================

void addSink(std::unique_ptr<Sink> sink)
{
	s_sinks.push_back(std::move(sink));
}

void removeSinks(SinkKind kind)
{
	std::erase_if(s_sinks, [kind](const std::unique_ptr<Sink>& s){ return s->kind() == kind; });
}

bool hasSink(SinkKind kind)
{
	return std::any_of(s_sinks.begin(), s_sinks.end(),
		[kind](const std::unique_ptr<Sink>& s){ return s->kind() == kind; });
}

void dispatch(std::string_view text)
{
	if (text.empty())
		return;

	const LogMode mode = getMode();
	for (const std::unique_ptr<Sink>& sink : s_sinks)
	{
		if (sink->accepts(mode))
			sink->write(text);
	}
}

} // namespace Log
