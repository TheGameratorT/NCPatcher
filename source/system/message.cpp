#include "message.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <functional>
#include <mutex>
#include <vector>

#include "log.hpp"
#include "../utils/json.hpp"

namespace fs = std::filesystem;

namespace ncp::msg {

namespace {

struct RecordedDiagnostic
{
	Level level;
	Diag code;
	std::string message;
	Location location;
};

Format s_format = Format::Human;
fs::path s_resultFile;
std::chrono::steady_clock::time_point s_start = std::chrono::steady_clock::now();
bool s_finished = false;

std::vector<RecordedDiagnostic> s_diagnostics;
std::vector<Artifact> s_artifacts;

// The compile phase runs on a thread pool, so progress and artifacts can arrive
// concurrently. One event has to be one line, so the whole emission is held.
//
// Recursive because the warning observer turns anything written to the log into
// a diagnostic, and code inside this file legitimately warns.
std::recursive_mutex s_mutex;
using Lock = std::lock_guard<std::recursive_mutex>;

const char* levelName(Level level)
{
	switch (level)
	{
	case Level::Error:   return "error";
	case Level::Warning: return "warning";
	default:             return "note";
	}
}

// One JSON object, one line, flushed: a caller reading the stream must be able
// to act on an event before the process exits.
void emitLine(const std::function<void(Json::Writer&)>& body)
{
	Json::Writer writer(std::cout);
	writer.beginObject();
	body(writer);
	writer.endObject();
	std::cout << '\n' << std::flush;
}

void writeLocation(Json::Writer& writer, const Location& location)
{
	if (!location.valid())
		return;

	writer.key("location").beginObject();
	writer.field("file", location.file);
	if (location.line > 0)
	{
		writer.field("line", location.line);
		writer.field("col", location.column);
	}
	if (!location.path.empty())
		writer.field("path", location.path);
	writer.endObject();
}

void writeArtifact(Json::Writer& writer, const Artifact& entry)
{
	writer.field("kind", entry.kind);
	if (!entry.proc.empty())
		writer.field("proc", entry.proc);
	if (entry.id >= 0)
		writer.field("id", entry.id);
	writer.field("action", entry.action);
	if (entry.size >= 0)
		writer.field("size", entry.size);
	if (entry.hasRamAddress)
		writer.key("ram-address").hex(entry.ramAddress);
	if (entry.fileId >= 0)
		writer.key("file-id").value(entry.fileId);
	if (!entry.name.empty())
		writer.field("name", entry.name);
}

long long elapsedMs()
{
	const auto now = std::chrono::steady_clock::now();
	return std::chrono::duration_cast<std::chrono::milliseconds>(now - s_start).count();
}

std::size_t diagnosticCount(Level level)
{
	std::size_t count = 0;
	for (const RecordedDiagnostic& diagnostic : s_diagnostics)
		count += diagnostic.level == level ? 1 : 0;
	return count;
}

void writeResultFile(std::string_view status, int exitCode, long long duration)
{
	std::ofstream file(s_resultFile);
	if (!file.is_open())
	{
		// A summary that cannot be written must not take the build down with
		// it: by the time this runs the ROM has already been patched.
		//
		// Reported straight to stderr rather than through the log, because this
		// runs while the diagnostic list is being walked and the log's warning
		// observer would try to append to it.
		std::cerr << "Warning: could not write the result file "
		          << s_resultFile.string() << "." << std::endl;
		return;
	}

	Json::Writer writer(file, 2);
	writer.beginObject();
	writer.field("schema", "ncpatcher.result/1");
	writer.field("status", status);
	writer.key("exit-code").value(exitCode);
	writer.key("duration-ms").value(duration);
	writer.key("errors").value(diagnosticCount(Level::Error));
	writer.key("warnings").value(diagnosticCount(Level::Warning));

	writer.key("diagnostics").beginArray();
	for (const RecordedDiagnostic& entry : s_diagnostics)
	{
		writer.beginObject();
		writer.field("level", levelName(entry.level));
		if (entry.code != Diag::None)
			writer.field("code", diagCode(entry.code));
		writer.field("message", entry.message);
		writeLocation(writer, entry.location);
		writer.endObject();
	}
	writer.endArray();

	writer.key("artifacts").beginArray();
	for (const Artifact& entry : s_artifacts)
	{
		writer.beginObject();
		writeArtifact(writer, entry);
		writer.endObject();
	}
	writer.endArray();

	writer.endObject();
	file << '\n';
}

} // namespace

void configure(Format newFormat, fs::path resultFile)
{
	s_format = newFormat;
	s_resultFile = std::move(resultFile);
	s_start = std::chrono::steady_clock::now();
	s_finished = false;
	s_diagnostics.clear();
	s_artifacts.clear();

	Log::setWarningObserver([](std::string_view text)
	{
		// Diag::None because a warning raised through Log carries no phase; the
		// ones that do are reported by diagnostic() directly.
		diagnostic(Level::Warning, Diag::None, text);
	});
}

Format format() { return s_format; }
bool isJson() { return s_format == Format::Json; }

void diagnostic(Level level, Diag code, std::string_view message, const Location& location)
{
	Lock lock(s_mutex);
	s_diagnostics.push_back({ level, code, std::string(message), location });

	if (s_format != Format::Json)
		return;

	emitLine([&](Json::Writer& writer)
	{
		writer.field("type", "diagnostic");
		writer.field("level", levelName(level));
		// Omitted rather than empty when there is no phase to name: a consumer
		// checking for the key gets a clean answer either way.
		if (code != Diag::None)
			writer.field("code", diagCode(code));
		writer.field("message", message);
		writeLocation(writer, location);
	});
}

void progress(std::string_view phase, std::size_t current, std::size_t total, std::string_view item)
{
	if (s_format != Format::Json)
		return;

	Lock lock(s_mutex);
	emitLine([&](Json::Writer& writer)
	{
		writer.field("type", "progress");
		writer.field("phase", phase);
		writer.field("current", current);
		writer.field("total", total);
		if (!item.empty())
			writer.field("item", item);
	});
}

void artifact(Artifact entry)
{
	Lock lock(s_mutex);

	if (s_format == Format::Json)
	{
		emitLine([&](Json::Writer& writer)
		{
			writer.field("type", "artifact");
			writeArtifact(writer, entry);
		});
	}

	s_artifacts.push_back(std::move(entry));
}

void finish(std::string_view status, int exitCode)
{
	Lock lock(s_mutex);
	if (s_finished)
		return;
	s_finished = true;

	const long long duration = elapsedMs();

	if (s_format == Format::Json)
	{
		emitLine([&](Json::Writer& writer)
		{
			writer.field("type", "result");
			writer.field("status", status);
			writer.key("exit-code").value(exitCode);
			writer.key("duration-ms").value(duration);
			writer.key("errors").value(diagnosticCount(Level::Error));
			writer.key("warnings").value(diagnosticCount(Level::Warning));
		});
	}

	if (!s_resultFile.empty())
		writeResultFile(status, exitCode, duration);
}

} // namespace ncp::msg
