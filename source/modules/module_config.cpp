#include "module_config.hpp"

#include <initializer_list>
#include <set>
#include <sstream>

#include "problems.hpp"
#include "../system/log.hpp"
#include "../system/except.hpp"

namespace fs = std::filesystem;

namespace ncp::modules {

std::string TargetRef::str() const
{
	if (!valid)
		return {};
	std::string out = arm9 ? "arm9" : "arm7";
	if (overlay >= 0)
		out += "(" + std::to_string(overlay) + ")";
	return out;
}

std::string TargetRef::regionName() const
{
	return overlay < 0 ? std::string("main") : "ov" + std::to_string(overlay);
}

bool parseTargetRef(std::string_view text, TargetRef& out, std::string& error)
{
	out = TargetRef();

	if (text.starts_with('!'))
	{
		out.locked = true;
		text.remove_prefix(1);
	}

	std::string_view proc = text;
	std::string_view overlay;

	const std::size_t open = text.find('(');
	if (open != std::string_view::npos)
	{
		if (!text.ends_with(')'))
		{
			error = "Missing the closing parenthesis around the overlay id.";
			return false;
		}
		proc = text.substr(0, open);
		overlay = text.substr(open + 1, text.size() - open - 2);
	}

	if (proc == "arm9")
		out.arm9 = true;
	else if (proc == "arm7")
		out.arm9 = false;
	else
	{
		std::ostringstream oss;
		oss << "Unknown processor " << OSTR(std::string(proc)) << ", expected "
		    << OSTRa("arm9") << " or " << OSTRa("arm7") << ".";
		error = oss.str();
		return false;
	}

	if (open == std::string_view::npos)
	{
		out.overlay = -1;
		out.valid = true;
		return true;
	}

	if (overlay.empty() || overlay.find_first_not_of("0123456789") != std::string_view::npos)
	{
		std::ostringstream oss;
		oss << "Overlay id " << OSTR(std::string(overlay)) << " is not a number.";
		error = oss.str();
		return false;
	}

	try {
		out.overlay = std::stoi(std::string(overlay));
	} catch (const std::exception&) {
		error = "Overlay id is out of range.";
		return false;
	}

	out.valid = true;
	return true;
}

const ComponentDef* ModuleDef::findComponent(std::string_view name) const
{
	for (const ComponentDef& component : components)
	{
		if (component.name == name)
			return &component;
	}
	return nullptr;
}

namespace {

void checkKeys(const cfg::Node& node, std::string_view what,
               std::initializer_list<std::string_view> known, Problems& problems)
{
	for (const auto& [key, value] : node.fields())
	{
		bool found = false;
		for (std::string_view candidate : known)
		{
			if (key == candidate)
			{
				found = true;
				break;
			}
		}
		if (found)
			continue;

		std::ostringstream oss;
		oss << "Unknown key " << OSTR(key) << " in " << what << "." OREASONNL "Expected one of: ";
		bool first = true;
		for (std::string_view candidate : known)
		{
			oss << (first ? "" : ", ") << candidate;
			first = false;
		}
		problems.add(value, oss.str());
	}
}

// A scalar or a sequence of scalars, both spelled the same way everywhere in
// these files: `sources: "source9/*"` and `sources: ["a", "b"]` are one entry
// and two.
std::vector<std::string> readStrings(const cfg::Node& node, Problems& problems)
{
	std::vector<std::string> out;
	if (!node.defined() || node.isNull())
		return out;

	if (node.isScalar())
	{
		out.push_back(node.asString());
		return out;
	}

	if (!node.isSequence())
	{
		problems.add(node, "Expected a string or a list of strings.");
		return out;
	}

	for (const cfg::Node& item : node.items())
	{
		if (!item.isScalar())
		{
			problems.add(item, "Expected a string.");
			continue;
		}
		out.push_back(item.asString());
	}

	return out;
}

std::vector<DefineText> readDefines(const cfg::Node& node, Problems& problems)
{
	std::vector<DefineText> out;
	if (!node.defined() || node.isNull())
		return out;

	// The mapping form -- `defines: {NAME: VALUE}` -- is what somebody writing a
	// numeric value reaches for first, and means the same as "NAME=VALUE".
	if (node.isMap())
	{
		for (const auto& [name, value] : node.fields())
			out.push_back({ name + "=" + value.asString(), value.mark() });
		return out;
	}

	for (const std::string& text : readStrings(node, problems))
		out.push_back({ text, node.mark() });

	return out;
}

// The mapping and the sequence-of-single-key-mappings forms both mean the same
// thing, and both are already written in the wild: YAML mappings do not keep
// duplicate keys, so the sequence form is the only one that can express a
// mistake like the same component named twice -- which is precisely why it has
// to be accepted and then diagnosed rather than rejected out of hand.
std::vector<std::pair<std::string, cfg::Node>> readEntries(
	const cfg::Node& node, std::string_view what, Problems& problems)
{
	std::vector<std::pair<std::string, cfg::Node>> out;
	if (!node.defined() || node.isNull())
		return out;

	if (node.isMap())
	{
		for (auto& [key, value] : node.fields())
			out.emplace_back(key, value);
		return out;
	}

	if (!node.isSequence())
	{
		std::ostringstream oss;
		oss << "Expected a mapping or a list of named " << what << ".";
		problems.add(node, oss.str());
		return out;
	}

	for (const cfg::Node& item : node.items())
	{
		if (item.isScalar())
		{
			// `- FadeFix` with nothing under it: a component that only exists
			// to be switched off.
			out.emplace_back(item.asString(), cfg::Node());
			continue;
		}

		if (!item.isMap())
		{
			std::ostringstream oss;
			oss << "Expected a named entry, as " << OSTRa("- name:") << ".";
			problems.add(item, oss.str());
			continue;
		}

		for (auto& [key, value] : item.fields())
			out.emplace_back(key, value);
	}

	return out;
}

// The body of one `targets:` entry, which may itself be written either as a
// mapping or as a list of single-key mappings.
void readTargetBody(ModuleTarget& target, const cfg::Node& node, Problems& problems)
{
	if (!node.defined() || node.isNull())
		return;

	if (node.isMap())
	{
		checkKeys(node, "a module target", { "includes", "sources" }, problems);
		target.includes = readStrings(node["includes"], problems);
		target.sources = readStrings(node["sources"], problems);
		return;
	}

	if (!node.isSequence())
	{
		problems.add(node, "Expected a mapping of " ANSI_bCYAN "includes" ANSI_RESET
			" and " ANSI_bCYAN "sources" ANSI_RESET ".");
		return;
	}

	for (const cfg::Node& item : node.items())
	{
		if (!item.isMap())
		{
			problems.add(item, "Expected a mapping of " ANSI_bCYAN "includes" ANSI_RESET
				" and " ANSI_bCYAN "sources" ANSI_RESET ".");
			continue;
		}

		checkKeys(item, "a module target", { "includes", "sources" }, problems);
		for (const std::string& value : readStrings(item["includes"], problems))
			target.includes.push_back(value);
		for (const std::string& value : readStrings(item["sources"], problems))
			target.sources.push_back(value);
	}
}

void readComponent(ComponentDef& component, const cfg::Node& node, Problems& problems)
{
	if (!node.defined() || node.isNull())
		return;

	if (!node.isMap())
	{
		problems.add(node, "Expected a mapping.");
		return;
	}

	component.mark = node.mark();

	static constexpr std::string_view KNOWN[] = {
		"target", "sources", "includes", "defines", "files", "requires" };

	for (auto& [key, value] : node.fields())
	{
		bool known = false;
		for (std::string_view candidate : KNOWN)
		{
			if (key == candidate)
			{
				known = true;
				break;
			}
		}

		if (!known)
		{
			// Deliberately kept rather than rejected. See ComponentDef::extra.
			component.extra.emplace_back(key, value);
			continue;
		}

		if (key == "target")
		{
			component.targetText = value.asString();
			std::string error;
			if (!parseTargetRef(component.targetText, component.target, error))
				problems.add(value, std::move(error));
		}
		else if (key == "sources")   component.sources = readStrings(value, problems);
		else if (key == "includes")  component.includes = readStrings(value, problems);
		else if (key == "defines")   component.defines = readDefines(value, problems);
		else if (key == "files")     component.files = readStrings(value, problems);
		else if (key == "requires")  component.requires_ = readStrings(value, problems);
	}
}

} // namespace

ModuleDef loadModuleFile(const fs::path& file, std::string key)
{
	ModuleDef out;
	out.key = std::move(key);
	out.file = file;
	out.dir = file.parent_path();
	out.document = std::make_shared<cfg::Document>(file);

	Problems problems;

	const cfg::Node root = out.document->root();
	if (!root.isMap())
		root.failType("a mapping");

	checkKeys(root, "a module", {
		"id", "name", "description", "authors", "repo",
		"defines", "targets", "components" }, problems);

	out.id = root["id"].asString("");
	out.name = root["name"].asString(out.id);
	out.description = root["description"].asString("");
	out.repo = root["repo"].asString("");
	out.authors = readStrings(root["authors"], problems);
	out.defines = readDefines(root["defines"], problems);

	for (const auto& [text, body] : readEntries(root["targets"], "targets", problems))
	{
		ModuleTarget target;
		target.text = text;
		target.mark = body.defined() ? body.mark() : root["targets"].mark();

		std::string error;
		if (!parseTargetRef(text, target.target, error))
		{
			problems.add(body.defined() ? body : root["targets"], std::move(error));
			continue;
		}

		readTargetBody(target, body, problems);
		out.targets.push_back(std::move(target));
	}

	std::set<std::string> seen;
	for (const auto& [name, body] : readEntries(root["components"], "components", problems))
	{
		if (!seen.insert(name).second)
		{
			// The prototype kept both and let them share one name, so a project
			// could no longer switch either on or off by itself, and `requires`
			// could not say which one it meant. One name, one component.
			std::ostringstream oss;
			oss << "Component " << OSTR(name) << " is declared more than once."
			    << OREASONNL "A component's name is how a project enables, disables and retargets it, "
			    << "so two of them cannot share one.";
			problems.add(body.defined() ? body : root["components"], oss.str());
			continue;
		}

		ComponentDef component;
		component.name = name;
		component.mark = body.defined() ? body.mark() : root["components"].mark();
		readComponent(component, body, problems);
		out.components.push_back(std::move(component));
	}

	std::ostringstream summary;
	summary << "Could not read the module " << OSTR(out.key) << ".";
	const std::string summaryText = summary.str();
	problems.raise(summaryText.c_str());

	return out;
}

} // namespace ncp::modules
