#pragma once

#include <cstddef>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>
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
extern const char* log_OREASON;

namespace Log {
// The two severity prefixes are function calls rather than constants so that
// tallying them costs nothing at the call site. Every warning in this program
// is written as `Log::out << OWARN << ...`, and the run summary (and the
// `{"type":"result","warnings":N}` event) has to be able to say how many
// there were without anyone remembering to increment a counter alongside.
[[nodiscard]] const char* warnPrefix();
[[nodiscard]] const char* errorPrefix();
}

#define OERROR ::Log::errorPrefix()
#define OWARN ::Log::warnPrefix()
#define OINFO log_OINFO
#define OREASON log_OREASON

#endif

enum class LogMode
{
	Both,
	Console,
	File
};

namespace Log {

class OutputStream : public std::ostream
{
public:
	OutputStream();
	~OutputStream() override;
};

extern OutputStream out;

// How the console output should be styled.
enum class ColorMode
{
	Auto,   // styled if the console can render it
	Always, // styled regardless, for a pipe that is going to a pager
	Never
};

void init();
void destroy();

// Chooses the console sink: whether it emits escapes, and which stream it
// writes to. `toStderr` is what --message-format json uses to leave stdout
// carrying nothing but the event stream.
//
// Call after init(), which installs the auto-detected default so that failures
// before the command line has even been parsed still reach a console.
void configureConsole(ColorMode color, bool toStderr);

// Collects file-destined output in memory until openLogFile() is called, which
// writes it out as the log's first lines. See BufferSink for why the path is
// not known yet at the point the first messages are written.
void beginBufferedLogFile();

// Replaces whatever file destination is installed, buffer included, with the
// named file. Throws if it cannot be opened.
void openLogFile(const std::filesystem::path& path);
void closeLogFile();

// Whether a real log file is open, as opposed to a buffer or nothing.
[[nodiscard]] bool logFileOpen();

void log(const std::string& str);
void info(const std::string& str);
void warn(const std::string& str);
void error(const std::string& str);

// A cargo-style milestone: `verb` right-aligned in a fixed column, bold green,
// then the message. This is the only thing a default build prints besides
// warnings, errors and the live block.
void step(std::string_view verb, const std::string& message);

// The verb column and message column step() uses, exported as
// NCPATCHER_LOG_COLUMN/NCPATCHER_LOG_INDENT so a hook can match them without
// hardcoding a number that might drift from this file.
[[nodiscard]] int stepColumnWidth();
[[nodiscard]] int stepMessageColumn();

// Brackets the milestones for one build target (arm7/arm9): a blank line (
// unless nothing has been printed yet, or the previous line already was one),
// then the target name indented under the milestone column. endGroup() does
// not print anything itself; it only notes that the next top-level step()
// needs a separating blank line first, so a build that stops mid-group never
// leaves a trailing blank line behind it.
void group(std::string_view name);
void endGroup();

// Routes everything written during its lifetime to the log file (and to a
// piped console, which has no live block to replace it) instead of the
// terminal. Restores whatever mode was active when it was constructed.
class FileOnly
{
public:
	FileOnly();
	~FileOnly();
private:
	LogMode m_prev;
};

// How many times the warning and error prefixes have been emitted this run.
[[nodiscard]] std::size_t warningCount();
// Of those, how many were emitted while console output was on: a warning
// written under Log::FileOnly reaches the log but never the terminal, so this
// is what the "Finished" milestone can actually claim to have shown.
[[nodiscard]] std::size_t consoleWarningCount();
[[nodiscard]] std::size_t errorCount();
void resetCounts();

// Called once per complete warning message, with the styling stripped and the
// prefix removed.
//
// Warnings are written as `Log::out << OWARN << ...` in three dozen places, and
// the machine-readable output needs the same text. Recognizing them here rather
// than rewriting every site is the difference between a small change and a
// sweeping one, and the prefix is already exactly the assertion "this is a
// warning". Errors are deliberately not observed: they are reported once,
// explicitly, with the phase and file location attached.
void setWarningObserver(std::function<void(std::string_view)> observer);

void setMode(LogMode mode);
[[nodiscard]] LogMode getMode();

// Whether the console can address the cursor. False when stdout is a pipe or a
// dumb terminal, in which case the progress display is skipped and the settled
// record is printed instead.
[[nodiscard]] bool terminalSupportsCursor();

// Whether the console sink installed by configureConsole() is emitting ANSI
// escapes, once ColorMode::Auto has been resolved one way or the other. A
// child process handed our pipe cannot answer this by checking its own
// isatty(), so it is exported to hooks as NCPATCHER_COLOR.
[[nodiscard]] bool consoleIsStyled();

// The size of the console in columns and rows. Falls back to {80, 24} when it
// cannot be queried, so a caller never has to special-case failure.
Coords terminalSize();

void showCursor(bool flag);

}
