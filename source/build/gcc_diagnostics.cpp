#include "gcc_diagnostics.hpp"

#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>

#include <yaml-cpp/yaml.h>

#include "../system/process.hpp"

namespace ncp::build {

namespace {

// JSON is a subset of YAML 1.2, so both formats are read by the reader that is
// already here rather than by a second one.
std::optional<std::vector<GccDiagnostic>> parseJsonDiagnostics(
	std::string_view output,
	std::string_view fallbackFile
)
{
	try
	{
		const YAML::Node root = YAML::Load(std::string(output));
		if (!root.IsSequence())
			return std::nullopt;

		std::vector<GccDiagnostic> diagnostics;
		std::function<bool(const YAML::Node&)> append = [&](const YAML::Node& node) {
			if (!node.IsMap())
				return false;
			const YAML::Node kindNode = node["kind"];
			const YAML::Node messageNode = node["message"];
			if (!kindNode.IsDefined() || !kindNode.IsScalar() ||
				!messageNode.IsDefined() || !messageNode.IsScalar())
				return false;

			const std::string kind = kindNode.as<std::string>();
			GccDiagnostic diagnostic;
			diagnostic.level = kind == "warning" ? msg::Level::Warning :
				(kind == "note" ? msg::Level::Note : msg::Level::Error);
			diagnostic.message = messageNode.as<std::string>();
			const YAML::Node option = node["option"];
			if (option.IsDefined() && option.IsScalar())
				diagnostic.message += " [" + option.as<std::string>() + ']';
			diagnostic.location.file = std::string(fallbackFile);

			const YAML::Node locations = node["locations"];
			if (locations.IsDefined() && locations.IsSequence() && locations.size() != 0)
			{
				const YAML::Node caret = locations[0]["caret"];
				if (caret.IsDefined() && caret.IsMap())
				{
					const YAML::Node file = caret["file"];
					const YAML::Node line = caret["line"];
					const YAML::Node displayColumn = caret["display-column"];
					const YAML::Node column = caret["column"];
					if (file.IsDefined() && file.IsScalar())
						diagnostic.location.file = file.as<std::string>();
					if (line.IsDefined() && line.IsScalar())
						diagnostic.location.line = line.as<int>();
					if (displayColumn.IsDefined() && displayColumn.IsScalar())
						diagnostic.location.column = displayColumn.as<int>();
					else if (column.IsDefined() && column.IsScalar())
						diagnostic.location.column = column.as<int>();
				}
			}

			diagnostics.push_back(std::move(diagnostic));
			const YAML::Node children = node["children"];
			if (children.IsDefined() && children.IsSequence())
				for (const YAML::Node& child : children)
					if (!append(child))
						return false;
			return true;
		};

		for (const YAML::Node& node : root)
			if (!append(node))
				return std::nullopt;
		return diagnostics;
	}
	catch (const YAML::Exception&)
	{
		return std::nullopt;
	}
}

// yaml-cpp throws rather than answering "absent" when a missing key is indexed
// again, so a SARIF document that simply lacks an optional member would be read
// as a document that is not SARIF at all. Every step down goes through here.
YAML::Node field(const YAML::Node& node, const char* key)
{
	if (!node.IsDefined() || !node.IsMap())
		return YAML::Node(YAML::NodeType::Undefined);
	return node[key];
}

// A SARIF physical location, which is the same shape wherever it appears.
void readSarifLocation(const YAML::Node& location, GccDiagnostic& diagnostic)
{
	const YAML::Node physical = field(location, "physicalLocation");
	if (!physical.IsDefined() || !physical.IsMap())
		return;

	const YAML::Node uri = field(field(physical, "artifactLocation"), "uri");
	if (uri.IsDefined() && uri.IsScalar())
		diagnostic.location.file = uri.as<std::string>();

	const YAML::Node region = field(physical, "region");
	if (!region.IsDefined() || !region.IsMap())
		return;
	const YAML::Node line = field(region, "startLine");
	const YAML::Node column = field(region, "startColumn");
	if (line.IsDefined() && line.IsScalar())
		diagnostic.location.line = line.as<int>();
	if (column.IsDefined() && column.IsScalar())
		diagnostic.location.column = column.as<int>();
}

std::optional<std::vector<GccDiagnostic>> parseSarifDiagnostics(
	std::string_view output,
	std::string_view fallbackFile
)
{
	try
	{
		const YAML::Node root = YAML::Load(std::string(output));
		if (!root.IsMap())
			return std::nullopt;
		const YAML::Node runs = field(root, "runs");
		if (!runs.IsDefined() || !runs.IsSequence())
			return std::nullopt;

		std::vector<GccDiagnostic> diagnostics;
		for (const YAML::Node& run : runs)
		{
			const YAML::Node results = field(run, "results");
			if (!results.IsDefined() || !results.IsSequence())
				continue;

			for (const YAML::Node& result : results)
			{
				const YAML::Node text = field(field(result, "message"), "text");
				if (!text.IsDefined() || !text.IsScalar())
					return std::nullopt;

				GccDiagnostic diagnostic;
				const YAML::Node level = field(result, "level");
				const std::string levelName = level.IsDefined() && level.IsScalar()
					? level.as<std::string>() : std::string("error");
				diagnostic.level = levelName == "warning" ? msg::Level::Warning :
					(levelName == "note" ? msg::Level::Note : msg::Level::Error);
				diagnostic.message = text.as<std::string>();

				// The JSON format had a separate `option` field; SARIF puts the
				// warning switch in `ruleId`, which for an ordinary error holds
				// the level word instead. A leading dash is what tells them
				// apart, and only a switch is worth showing.
				const YAML::Node rule = field(result, "ruleId");
				if (rule.IsDefined() && rule.IsScalar())
				{
					const std::string id = rule.as<std::string>();
					if (!id.empty() && id.front() == '-')
						diagnostic.message += " [" + id + ']';
				}

				diagnostic.location.file = std::string(fallbackFile);
				const YAML::Node locations = field(result, "locations");
				if (locations.IsDefined() && locations.IsSequence() && locations.size() != 0)
					readSarifLocation(locations[0], diagnostic);

				diagnostics.push_back(std::move(diagnostic));

				// What `children` was in the JSON format: the notes under a
				// diagnostic, each with its own location. A related location
				// carrying no message is another caret for the same message,
				// not a note, so it is not one of these.
				const YAML::Node related = field(result, "relatedLocations");
				if (!related.IsDefined() || !related.IsSequence())
					continue;
				for (const YAML::Node& location : related)
				{
					const YAML::Node note = field(field(location, "message"), "text");
					if (!note.IsDefined() || !note.IsScalar())
						continue;

					GccDiagnostic child;
					child.level = msg::Level::Note;
					child.message = note.as<std::string>();
					child.location.file = std::string(fallbackFile);
					readSarifLocation(location, child);
					diagnostics.push_back(std::move(child));
				}
			}
		}
		return diagnostics;
	}
	catch (const YAML::Exception&)
	{
		return std::nullopt;
	}
}

bool compilerAccepts(const std::string& compiler, const char* format,
                     const std::filesystem::path& source, const std::filesystem::path& output)
{
	std::string command = compiler;
	command += " -fdiagnostics-format=";
	command += format;
	command += " -S \"";
	command += source.string();
	command += "\" -o \"";
	command += output.string();
	command += "\"";

	std::ostringstream ignored;
	return Process::start(command.c_str(), {}, &ignored) == 0;
}

} // namespace

DiagnosticsFormat detectDiagnosticsFormat(const std::string& compiler,
                                          const std::filesystem::path& probeDir)
{
	static std::mutex mutex;
	static std::map<std::string, DiagnosticsFormat> cache;

	const std::lock_guard lock(mutex);
	if (const auto found = cache.find(compiler); found != cache.end())
		return found->second;

	// An empty translation unit, which every compiler accepts and which costs
	// nothing to compile. The alternative is asking the compiler about itself,
	// and it will happily accept a format on a command line that does no work
	// and then refuse the same format when there is a file to report about.
	DiagnosticsFormat format = DiagnosticsFormat::Text;
	std::error_code ec;
	std::filesystem::create_directories(probeDir, ec);
	const std::filesystem::path source = probeDir / "ncp_diagnostics_probe.c";
	const std::filesystem::path output = probeDir / "ncp_diagnostics_probe.s";
	{
		std::ofstream probe(source);
		if (probe.is_open())
		{
			probe.close();
			if (compilerAccepts(compiler, "sarif-stderr", source, output))
				format = DiagnosticsFormat::Sarif;
			else if (compilerAccepts(compiler, "json", source, output))
				format = DiagnosticsFormat::Json;
		}
	}
	std::filesystem::remove(source, ec);
	std::filesystem::remove(output, ec);

	cache.emplace(compiler, format);
	return format;
}

std::string diagnosticsFormatFlag(DiagnosticsFormat format)
{
	switch (format)
	{
	case DiagnosticsFormat::Sarif: return " -fdiagnostics-format=sarif-stderr";
	case DiagnosticsFormat::Json:  return " -fdiagnostics-format=json";
	case DiagnosticsFormat::Text:  break;
	}
	return {};
}

std::optional<std::vector<GccDiagnostic>> parseGccDiagnostics(
	std::string_view output,
	std::string_view fallbackFile,
	DiagnosticsFormat format
)
{
	switch (format)
	{
	case DiagnosticsFormat::Sarif: return parseSarifDiagnostics(output, fallbackFile);
	case DiagnosticsFormat::Json:  return parseJsonDiagnostics(output, fallbackFile);
	case DiagnosticsFormat::Text:  break;
	}
	return std::nullopt;
}

std::string formatGccDiagnostics(const std::vector<GccDiagnostic>& diagnostics)
{
	std::ostringstream out;
	for (const GccDiagnostic& diagnostic : diagnostics)
	{
		if (diagnostic.location.valid())
		{
			out << diagnostic.location.file;
			if (diagnostic.location.line > 0)
				out << ':' << diagnostic.location.line << ':' << diagnostic.location.column;
			out << ": ";
		}

		switch (diagnostic.level)
		{
		case msg::Level::Error:   out << "error: "; break;
		case msg::Level::Warning: out << "warning: "; break;
		case msg::Level::Note:    out << "note: "; break;
		}
		out << diagnostic.message << '\n';
	}
	return out.str();
}

} // namespace ncp::build
