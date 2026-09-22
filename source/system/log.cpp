#include "log.hpp"

#include <cstdlib>
#include <iostream>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif

#include "../utils/types.hpp"
#include "ansi.hpp"
#include "log_sink.hpp"

// What log_OWARN looks like once its styling has been stripped. Kept next to
// the definition below so the two cannot drift apart.
#define WARN_PLAIN_PREFIX "warning: "

const char* log_OERROR = ANSI_bRED "error: " ANSI_RESET;
const char* log_OWARN = ANSI_bYELLOW "warning: " ANSI_RESET;
const char* log_OREASON = "   -->  ";

// The width of the right-aligned verb column in Log::step(), matching what
// the live block's own header uses so a milestone lines up with it.
static constexpr int STEP_COLUMN_WIDTH = 12;
// Where step()'s message starts: the verb column plus the two spaces after
// it. log_OINFO indents to the same column, so a detail line printed under a
// milestone (verbose tables, sub-lines) lines up under the message rather
// than under the verb.
static constexpr int STEP_MESSAGE_COLUMN = STEP_COLUMN_WIDTH + 2;

// A plain indent, built from STEP_MESSAGE_COLUMN so the two cannot drift
// apart. Reads as subordinate to the milestone above it rather than as its
// own severity, which is what the old bracketed "[Info]" implied.
static const std::string s_infoIndent(STEP_MESSAGE_COLUMN, ' ');
const char* log_OINFO = s_infoIndent.c_str();

namespace Log {

static LogMode logMode = LogMode::Both;
static bool xyCapabilityAvailable = true;

// Set once the console sink has been chosen explicitly. A plain sink means
// there is nothing to move a cursor on even where the terminal would allow it,
// which is what --color never and --message-format json rely on.
static bool cursorDisabled = false;

// True only for a real file, so that a caller can tell "the log went nowhere"
// from "the log is still in the buffer".
static bool fileOpen = false;

static std::size_t warningsEmitted = 0;
static std::size_t consoleWarningsEmitted = 0;
static std::size_t errorsEmitted = 0;

const char* warnPrefix()
{
	warningsEmitted++;
	// A warning printed while in FileOnly mode (or otherwise not going to the
	// console) reaches the log but not the terminal, so the "Finished" tally
	// has to be able to tell the two counts apart rather than promise a
	// number the console never showed.
	if (logMode != LogMode::File)
		consoleWarningsEmitted++;
	return log_OWARN;
}

const char* errorPrefix()
{
	errorsEmitted++;
	return log_OERROR;
}

std::size_t warningCount() { return warningsEmitted; }
std::size_t consoleWarningCount() { return consoleWarningsEmitted; }
std::size_t errorCount() { return errorsEmitted; }

void resetCounts()
{
	warningsEmitted = 0;
	consoleWarningsEmitted = 0;
	errorsEmitted = 0;
}

// Line-shape tracking for group()/endGroup(): whether anything has been
// printed yet, whether the last line written ended up empty, and whether a
// group was closed without a top-level step() having followed it. All three
// are about layout, not content, so they are tracked once here rather than at
// every call site that might need a separating blank line.
static bool s_anyOutput = false;
static bool s_atLineStart = true;
static bool s_lastLineEmpty = false;
static bool s_inGroup = false;
static bool s_afterGroup = false;

// Scans a flushed chunk for where lines start and end, so group() can tell
// whether it needs to add a blank line or whether one is already there.
// ANSI escapes never contain '\n', so they cannot be mistaken for line
// content here.
static void trackLineShape(const std::string& text)
{
	if (text.empty())
		return;
	s_anyOutput = true;
	for (char c : text)
	{
		if (c == '\n')
		{
			s_lastLineEmpty = s_atLineStart;
			s_atLineStart = true;
		}
		else
		{
			s_atLineStart = false;
		}
	}
}

static std::function<void(std::string_view)> warningObserver;

void setWarningObserver(std::function<void(std::string_view)> observer)
{
	warningObserver = std::move(observer);
}

// Recognizes a warning by its prefix and hands on what follows it.
static void observe(const std::string& text)
{
	if (!warningObserver)
		return;

	const std::string plain = Ansi::strip(text);
	std::string_view rest(plain);
	if (!rest.starts_with(WARN_PLAIN_PREFIX))
		return;

	rest.remove_prefix(std::string_view(WARN_PLAIN_PREFIX).size());
	while (!rest.empty() && (rest.back() == '\n' || rest.back() == '\r'))
		rest.remove_suffix(1);

	warningObserver(rest);
}

// Collects everything streamed into Log::out until a flush, then hands the
// whole chunk to the sinks.
//
// It deliberately knows nothing about ANSI, files or consoles: a message is
// rendered once, here, and each sink decides what to do with it. Buffering to a
// flush is what keeps a styled line whole, because a sink that has to
// translate escapes into console attributes cannot do so if the escape and the
// text it applies to arrive in separate calls.
class OutputStreamBuffer : public std::stringbuf
{
public:
	~OutputStreamBuffer() override
	{
		pubsync();
	}

	int sync() override
	{
		const std::string text = str();
		observe(text);
		trackLineShape(text);
		dispatch(text);
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
	// A dumb terminal (or none at all, i.e. a pipe) cannot render the escapes
	// the progress display relies on, so it falls back to the plain, settled
	// record instead. This used to be decided by actually querying the cursor
	// position, which cost a stdin read and, against anything unresponsive, a
	// one second stall on every startup; the progress display no longer needs
	// to know where the cursor is, only whether it can be moved at all.
	const char* term = std::getenv("TERM");
	xyCapabilityAvailable = isatty(STDOUT_FILENO) && !(term && std::string_view(term) == "dumb");
#endif

	installConsoleSink();
}

void destroy()
{
	closeLogFile();
}

void beginBufferedLogFile()
{
	removeSinks(SinkKind::File);
	addSink(std::make_unique<BufferSink>());
}

void openLogFile(const std::filesystem::path& path)
{
	// Taken before the sink is built, so that a file that cannot be opened
	// leaves the buffer emptied rather than replayed into the next attempt.
	const std::string pending = takeBufferedLog();

	auto sink = std::make_unique<FileSink>(path);
	if (!pending.empty())
		sink->write(pending);

	removeSinks(SinkKind::File);
	addSink(std::move(sink));
	fileOpen = true;
}

bool logFileOpen()
{
	return fileOpen;
}

void closeLogFile()
{
	removeSinks(SinkKind::File);
	fileOpen = false;
}

void log(const std::string& str)
{
	out << str << std::endl;
}

void info(const std::string& str)
{
	out << str << std::endl;
}

void warn(const std::string& str)
{
	out << OWARN << str << std::endl;
}

void error(const std::string& str)
{
	out << OERROR << str << std::endl;
}

void step(std::string_view verb, const std::string& message)
{
	// A step() reached while not inside a group, right after one closed, is
	// the point a target's milestones hand back to the top-level ones (e.g.
	// "Writing" after the last target's "Patching"). That transition gets its
	// own blank line, same as between two groups; anything else does not.
	if (!s_inGroup && s_afterGroup)
	{
		if (!s_lastLineEmpty)
			out << '\n';
		s_afterGroup = false;
	}

	const int pad = STEP_COLUMN_WIDTH - int(verb.size());
	out << ANSI_bGREEN;
	for (int i = 0; i < pad; i++)
		out << ' ';
	out << verb << ANSI_RESET << "  " << message << std::endl;
}

int stepColumnWidth() { return STEP_COLUMN_WIDTH; }
int stepMessageColumn() { return STEP_MESSAGE_COLUMN; }

void group(std::string_view name)
{
	s_afterGroup = false;
	if (s_anyOutput && !s_lastLineEmpty)
		out << '\n';
	out << "  " << ANSI_bCYAN << name << ANSI_RESET << std::endl;
	s_inGroup = true;
}

void endGroup()
{
	s_inGroup = false;
	s_afterGroup = true;
}

FileOnly::FileOnly() :
	m_prev(getMode())
{
	setMode(LogMode::File);
}

FileOnly::~FileOnly()
{
	setMode(m_prev);
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
	if (cursorDisabled)
		return false;
#ifdef _WIN32
	return true;
#else
	return xyCapabilityAvailable;
#endif
}

void configureConsole(ColorMode color, bool toStderr)
{
	bool styled;
	switch (color)
	{
	case ColorMode::Always: styled = true; break;
	case ColorMode::Never:  styled = false; break;
	default:
		// Auto: the same question init() already answered. A console whose
		// cursor cannot be addressed is also one whose escapes are unlikely to
		// mean anything (a pipe, a file, a CI job).
		styled = hasSink(SinkKind::Terminal);
		break;
	}

	removeSinks(SinkKind::Terminal);
	removeSinks(SinkKind::Plain);

	if (styled)
	{
		addSink(std::make_unique<TerminalSink>(toStderr));
		cursorDisabled = toStderr;
	}
	else
	{
		addSink(std::make_unique<PlainSink>(toStderr));
		cursorDisabled = true;
	}
}

bool consoleIsStyled()
{
	return hasSink(SinkKind::Terminal);
}

#ifdef _WIN32

Coords terminalSize()
{
	HANDLE hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
	if (hStdOut != INVALID_HANDLE_VALUE)
	{
		CONSOLE_SCREEN_BUFFER_INFO cbsi;
		if (GetConsoleScreenBufferInfo(hStdOut, &cbsi))
		{
			const SMALL_RECT& win = cbsi.srWindow;
			return { int(win.Right - win.Left) + 1, int(win.Bottom - win.Top) + 1 };
		}
	}
	return { 80, 24 };
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

Coords terminalSize()
{
	struct winsize ws;
	if (xyCapabilityAvailable && ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0)
		return { ws.ws_col, ws.ws_row };
	return { 80, 24 };
}

void showCursor(bool flag)
{
	if (!xyCapabilityAvailable)
		return;

	const LogMode previousMode = getMode();
	setMode(LogMode::Console);
	out << (flag ? "\x1b[?25h" : "\x1b[?25l") << std::flush;
	setMode(previousMode);
}

#endif

}
