#include "module_dump.hpp"

#include <algorithm>
#include <sstream>

#include "../system/log.hpp"
#include "../system/except.hpp"
#include "../utils/json.hpp"

namespace fs = std::filesystem;

namespace ncp::modules {

namespace {

// The names worth showing after a lookup misses.
//
// A module with sixty-seven components makes "expected one of" useless as a
// complete list, so anything sharing text with what was typed comes first and
// the rest is a count.
std::string suggest(const std::vector<std::string>& names, const std::string& typed)
{
	std::string lowered = typed;
	for (char& c : lowered)
		c = char(std::tolower(static_cast<unsigned char>(c)));

	std::vector<std::string> near;
	for (const std::string& name : names)
	{
		std::string candidate = name;
		for (char& c : candidate)
			c = char(std::tolower(static_cast<unsigned char>(c)));
		if (!lowered.empty() && candidate.find(lowered) != std::string::npos)
			near.push_back(name);
	}

	const std::vector<std::string>& shown = near.empty() ? names : near;

	constexpr std::size_t LIMIT = 10;
	std::ostringstream oss;
	for (std::size_t i = 0; i < shown.size() && i < LIMIT; i++)
		oss << (i == 0 ? "" : ", ") << shown[i];
	if (shown.size() > LIMIT)
		oss << ", and " << (shown.size() - LIMIT) << " more";

	return oss.str();
}

const char* originName(TargetOrigin origin)
{
	switch (origin)
	{
	case TargetOrigin::Project:         return "project";
	case TargetOrigin::OverrideRefused: return "module (the override was refused)";
	default:                            return "module";
	}
}

// YAML that this program has no opinion about, re-emitted as JSON.
//
// Scalars are resolved the way YAML would: a number stays a number, a boolean
// stays a boolean, and everything else is a string. Quoting is not preserved,
// because by the time a node reaches here nothing remembers it.
void writeYaml(Json::Writer& writer, const YAML::Node& node)
{
	switch (node.Type())
	{
	case YAML::NodeType::Map:
		writer.beginObject();
		for (auto it = node.begin(); it != node.end(); ++it)
		{
			writer.key(it->first.Scalar());
			writeYaml(writer, it->second);
		}
		writer.endObject();
		return;

	case YAML::NodeType::Sequence:
		writer.beginArray();
		for (const YAML::Node& item : node)
			writeYaml(writer, item);
		writer.endArray();
		return;

	case YAML::NodeType::Scalar:
	{
		const std::string& text = node.Scalar();
		if (text == "true" || text == "True")   { writer.value(true); return; }
		if (text == "false" || text == "False") { writer.value(false); return; }

		try {
			std::size_t consumed = 0;
			const long long number = std::stoll(text, &consumed, 0);
			if (consumed == text.size())
			{
				writer.value(number);
				return;
			}
		} catch (const std::exception&) {}

		writer.value(text);
		return;
	}

	default:
		writer.null();
		return;
	}
}

void writeDefines(Json::Writer& writer, const std::vector<ResolvedDefine>& defines)
{
	writer.beginArray();
	for (const ResolvedDefine& define : defines)
	{
		writer.beginObject();
		writer.field("name", define.name);
		if (define.hasValue)
			writer.field("value", define.value);
		else
			{ writer.key("value"); writer.null(); }
		writer.field("origin", define.origin);
		writer.endObject();
	}
	writer.endArray();
}

void writePaths(Json::Writer& writer, const std::vector<fs::path>& paths)
{
	writer.beginArray();
	for (const fs::path& path : paths)
		writer.value(path.generic_string());
	writer.endArray();
}

void writeContribution(Json::Writer& writer, const TargetContribution& contribution)
{
	writer.beginObject();

	writer.key("includes");
	writePaths(writer, contribution.includes);

	writer.key("defines");
	writeDefines(writer, contribution.defines);

	writer.key("regions");
	writer.beginArray();
	for (const auto& [destination, sources] : contribution.regionSources)
	{
		writer.beginObject();
		writer.field("dest", destination < 0 ? std::string("main") : "ov" + std::to_string(destination));
		if (destination >= 0)
			writer.field("overlay", destination);
		writer.key("sources");
		writePaths(writer, sources);
		writer.endObject();
	}
	writer.endArray();

	writer.endObject();
}

} // namespace

void writeDump(std::ostream& out, const ModuleGraph& graph)
{
	Json::Writer writer(out, 2);

	writer.beginObject();
	writer.field("schema", "ncpatcher.modules/1");
	writer.field("dir", graph.directory().generic_string());

	writer.key("modules");
	writer.beginArray();

	for (const ResolvedModule& module : graph.modules())
	{
		writer.beginObject();
		writer.field("key", module.key);
		writer.field("id", module.id);
		writer.field("name", module.name);
		writer.field("enabled", module.def != nullptr);

		if (module.def == nullptr)
		{
			// Switched off in the project, so its file was never read. Saying
			// so is more useful than leaving it out: a generator that wants to
			// know why a module contributed nothing has its answer.
			writer.endObject();
			continue;
		}

		if (!module.description.empty())
			writer.field("description", module.description);
		if (!module.repo.empty())
			writer.field("repo", module.repo);
		writer.field("authors", module.authors);
		writer.field("dir", module.dir.generic_string());
		writer.field("file", module.file.generic_string());

		writer.key("components");
		writer.beginArray();
		for (const ResolvedComponent& component : module.components)
		{
			writer.beginObject();
			writer.field("name", component.name);
			writer.field("enabled", component.enabled);
			if (!component.enabled && !component.disabledReason.empty())
				writer.field("disabled-because", component.disabledReason);

			writer.key("target");
			if (component.target.valid)
			{
				writer.beginObject();
				writer.field("proc", component.target.arm9 ? "arm9" : "arm7");
				if (component.target.overlay >= 0)
					writer.field("overlay", component.target.overlay);
				else
					{ writer.key("overlay"); writer.null(); }
				writer.field("locked", component.target.locked);
				writer.field("origin", originName(component.targetOrigin));
				writer.endObject();
			}
			else
			{
				writer.null();
			}

			writer.key("defines");
			writeDefines(writer, component.defines);

			writer.key("sources");
			writePaths(writer, component.sources);

			writer.field("files", component.files);
			writer.field("requires", component.requires_);

			if (!component.def->extra.empty())
			{
				writer.key("extra");
				writer.beginObject();
				for (const auto& [key, value] : component.def->extra)
				{
					writer.key(key);
					writeYaml(writer, value.yaml());
				}
				writer.endObject();
			}

			writer.key("location");
			writer.beginObject();
			writer.field("file", module.file.generic_string());
			writer.field("line", component.def->mark.line);
			writer.field("col", component.def->mark.column);
			writer.endObject();

			writer.endObject();
		}
		writer.endArray();

		writer.endObject();
	}
	writer.endArray();

	writer.key("targets");
	writer.beginObject();
	writer.key("arm7");
	writeContribution(writer, graph.contribution(false));
	writer.key("arm9");
	writeContribution(writer, graph.contribution(true));
	writer.endObject();

	writer.endObject();
	out << std::endl;
}

void writeList(std::ostream& out, const ModuleGraph& graph)
{
	if (graph.modules().empty())
	{
		out << "No modules are enabled." << std::endl;
		return;
	}

	for (const ResolvedModule& module : graph.modules())
	{
		out << (module.def != nullptr ? "  [x] " : "  [ ] ") << module.key;
		if (!module.id.empty() && module.id != module.key)
			out << "  (" << module.id << ")";

		if (module.def == nullptr)
		{
			out << "  -- disabled\n";
			continue;
		}

		std::string procs;
		if (module.targetsArm9) procs = "arm9";
		if (module.targetsArm7) procs += procs.empty() ? "arm7" : ", arm7";
		out << "  -- " << procs;

		std::size_t enabled = 0;
		for (const ResolvedComponent& component : module.components)
			enabled += component.enabled ? 1 : 0;
		if (!module.components.empty())
			out << ", " << enabled << '/' << module.components.size() << " components";
		out << '\n';

		if (!module.description.empty())
			out << "      " << module.description << '\n';

		for (const ResolvedComponent& component : module.components)
		{
			if (component.enabled)
				continue;
			out << "      off: " << component.name;
			if (!component.disabledReason.empty())
				out << "  (" << component.disabledReason << ")";
			out << '\n';
		}
	}

	out << std::flush;
}

void writeExplanation(std::ostream& out, const ModuleGraph& graph, const std::string& what)
{
	const std::size_t dot = what.find('.');
	const std::string moduleName = dot == std::string::npos ? what : what.substr(0, dot);
	const std::string componentName = dot == std::string::npos ? std::string() : what.substr(dot + 1);

	const ResolvedModule* module = graph.find(moduleName);
	if (module == nullptr)
	{
		std::ostringstream oss;
		oss << "No module called " << OSTR(moduleName) << " is in this project.";
		if (!graph.modules().empty())
		{
			std::vector<std::string> names;
			for (const ResolvedModule& candidate : graph.modules())
				names.push_back(candidate.key);
			oss << OREASONNL "Did you mean: " << suggest(names, moduleName);
		}
		throw ncp::exception(oss.str());
	}

	if (componentName.empty())
	{
		out << "Module " << module->key;
		if (!module->id.empty())
			out << " (" << module->id << ")";
		out << "\n";
		if (!module->name.empty() && module->name != module->key)
			out << "  name:        " << module->name << '\n';
		if (!module->description.empty())
			out << "  about:       " << module->description << '\n';
		if (!module->authors.empty())
		{
			out << "  authors:     ";
			for (std::size_t i = 0; i < module->authors.size(); i++)
				out << (i == 0 ? "" : ", ") << module->authors[i];
			out << '\n';
		}
		if (!module->repo.empty())
			out << "  repo:        " << module->repo << '\n';

		if (module->def == nullptr)
		{
			out << "  enabled:     no -- the project switched it off, so its module.yaml was not read\n";
			out << std::flush;
			return;
		}

		out << "  enabled:     yes\n";
		out << "  file:        " << module->file.generic_string() << '\n';
		out << "  defines:     MODULE_" << [&] {
			std::string upper = module->id;
			for (char& c : upper)
				c = char(std::toupper(static_cast<unsigned char>(c)));
			return upper;
		}() << '\n';
		out << "  components:  " << module->components.size() << '\n';
		for (const ResolvedComponent& component : module->components)
		{
			out << "    " << (component.enabled ? "[x] " : "[ ] ") << component.name;
			if (component.target.valid)
				out << "  -> " << component.target.str();
			out << '\n';
		}
		out << std::flush;
		return;
	}

	const ResolvedComponent* component = module->findComponent(componentName);
	if (component == nullptr)
	{
		std::ostringstream oss;
		oss << "Module " << OSTR(module->key) << " has no component " << OSTR(componentName) << ".";
		if (!module->components.empty())
		{
			std::vector<std::string> names;
			for (const ResolvedComponent& candidate : module->components)
				names.push_back(candidate.name);
			oss << OREASONNL "Did you mean: " << suggest(names, componentName);
		}
		throw ncp::exception(oss.str());
	}

	out << "Component " << module->id << '.' << component->name << '\n';
	out << "  module:      " << module->key << " (" << module->file.generic_string() << ")\n";

	if (component->def->mark.valid)
		out << "  declared at: " << module->file.filename().generic_string()
		    << ':' << component->def->mark.line << ':' << component->def->mark.column << '\n';

	out << "  enabled:     " << (component->enabled ? "yes" : "no");
	if (!component->enabled && !component->disabledReason.empty())
		out << " -- " << component->disabledReason;
	out << '\n';

	if (component->target.valid)
	{
		out << "  target:      " << component->target.str()
		    << "  (from the " << originName(component->targetOrigin) << ")\n";
		if (!component->declaredTarget.empty() && component->declaredTarget != component->target.str())
			out << "  declared as: " << component->declaredTarget << '\n';
	}
	else
	{
		out << "  target:      none -- it contributes defines only\n";
	}

	if (!component->requires_.empty())
	{
		out << "  requires:    ";
		for (std::size_t i = 0; i < component->requires_.size(); i++)
			out << (i == 0 ? "" : ", ") << component->requires_[i];
		out << '\n';
	}

	for (const ResolvedDefine& define : component->defines)
	{
		out << "  defines:     " << define.name;
		if (define.hasValue)
			out << " = " << define.value;
		out << '\n';
	}

	for (const fs::path& source : component->sources)
		out << "  source:      " << source.generic_string() << '\n';

	for (const std::string& file : component->files)
		out << "  file:        " << file << '\n';

	for (const auto& extra : component->def->extra)
		out << "  " << extra.first << ": passed through to the module dump\n";

	out << std::flush;
}

} // namespace ncp::modules
