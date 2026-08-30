#pragma once

#include <filesystem>
#include <iosfwd>
#include <memory>
#include <string>
#include <string_view>

#include "log.hpp"

namespace Log {

enum class SinkKind
{
	Terminal, // a console that can render styling and address the cursor
	Plain,    // a console that cannot, or is not a console at all
	File,     // the log file
};

// A destination for log output.
//
// Text arrives with its ANSI escapes intact and each sink decides what they
// mean: a terminal renders them, everything else drops them. Keeping that
// decision inside the sink is what lets a structured sink be added later
// without the other destinations having to know.
class Sink
{
public:
	virtual ~Sink() = default;

	virtual void write(std::string_view text) = 0;
	[[nodiscard]] virtual SinkKind kind() const = 0;

	// LogMode is how callers say who a message is *for*: BuildLogger's live
	// progress is Console-only, its final summary is File-only.
	[[nodiscard]] virtual bool accepts(LogMode mode) const = 0;
};

// Styling-capable console. Escapes pass through untouched on POSIX; on Windows
// they are translated into console attributes, since the console does not
// interpret them itself.
//
// This is the sink that takes LogMode::Console, because everything sent that
// way is transient (animation frames, characters rewritten in place) and
// only means anything where the cursor can be moved.
class TerminalSink final : public Sink
{
public:
	// `useStderr` moves the human log off stdout, which --message-format json
	// needs so that its event stream is the only thing a caller has to parse.
	explicit TerminalSink(bool useStderr = false);
	~TerminalSink() override;

	void write(std::string_view text) override;
	[[nodiscard]] SinkKind kind() const override { return SinkKind::Terminal; }
	[[nodiscard]] bool accepts(LogMode mode) const override { return mode != LogMode::File; }

private:
	std::ostream* m_stream;
};

// Console without styling or cursor control: a pipe, a file, a CI job's stdout.
//
// It takes LogMode::File rather than LogMode::Console on purpose. With no
// cursor to move there is no progress display to draw, so what this sink shows
// is the same settled record that goes to the log file, which is why a piped
// build still ends up with one line per source file.
class PlainSink final : public Sink
{
public:
	explicit PlainSink(bool useStderr = false);

	void write(std::string_view text) override;
	[[nodiscard]] SinkKind kind() const override { return SinkKind::Plain; }
	[[nodiscard]] bool accepts(LogMode mode) const override { return mode != LogMode::Console; }

private:
	std::ostream* m_stream;
};

// Holds output in memory until a log file path is known.
//
// The log belongs in the project's build directory, and where that is only
// becomes clear once the configuration has been read, which for a v1 project
// is after the pre-build commands have run, since those are allowed to generate
// the target files. Everything logged before that point is kept here and
// written to the file as its first lines, so the log still starts at the
// beginning. A run that never gets far enough to have a build directory drops
// the buffer; its output was on the console regardless.
//
// Reported as SinkKind::File, because that is what it stands in for.
class BufferSink final : public Sink
{
public:
	void write(std::string_view text) override;
	[[nodiscard]] SinkKind kind() const override { return SinkKind::File; }
	[[nodiscard]] bool accepts(LogMode mode) const override { return mode != LogMode::Console; }
};

// Everything a BufferSink has collected so far, clearing it. Empty when none
// was installed.
[[nodiscard]] std::string takeBufferedLog();

class FileSink final : public Sink
{
public:
	// Throws if the file cannot be opened for writing.
	explicit FileSink(const std::filesystem::path& path);
	~FileSink() override;

	void write(std::string_view text) override;
	[[nodiscard]] SinkKind kind() const override { return SinkKind::File; }
	[[nodiscard]] bool accepts(LogMode mode) const override { return mode != LogMode::Console; }

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

// Sink registry.
void addSink(std::unique_ptr<Sink> sink);
void removeSinks(SinkKind kind);
[[nodiscard]] bool hasSink(SinkKind kind);

// Hands text to every registered sink that accepts the current LogMode.
void dispatch(std::string_view text);

#ifdef _WIN32
// ANSI color code (30-37 foreground, 40-47 background) to the color bits the
// Windows console uses, which order red and blue the other way round. Shared
// with the cursor API, which paints attributes directly.
[[nodiscard]] int ansiColorToConsole(int ansiCode);
#endif

} // namespace Log
