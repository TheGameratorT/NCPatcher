#include "gcc_diagnostics.hpp"

#include <functional>
#include <sstream>

#include <yaml-cpp/yaml.h>

namespace ncp::build {

std::optional<std::vector<GccDiagnostic>> parseGccDiagnostics(
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
