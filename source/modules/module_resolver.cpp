#include "module_resolver.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>

#include "problems.hpp"
#include "../system/log.hpp"
#include "../system/except.hpp"
#include "../utils/glob.hpp"

namespace fs = std::filesystem;

namespace ncp::modules {

namespace {

bool validId(std::string_view id)
{
	if (id.empty())
		return false;
	if (!std::isalpha(static_cast<unsigned char>(id.front())))
		return false;
	for (char c : id)
	{
		if (!std::isalnum(static_cast<unsigned char>(c)))
			return false;
	}
	return true;
}

std::string upper(std::string_view text)
{
	std::string out(text);
	for (char& c : out)
		c = char(std::toupper(static_cast<unsigned char>(c)));
	return out;
}

// "Coop.SpikeBassFix", the name the dump and `modules explain` use.
std::string qualify(const ResolvedModule& module, const ResolvedComponent& component)
{
	return module.id + "." + component.name;
}

void splitDefine(std::string_view text, std::string& name, std::string& value, bool& hasValue)
{
	const std::size_t equals = text.find('=');
	if (equals == std::string_view::npos)
	{
		name = std::string(text);
		value.clear();
		hasValue = false;
		return;
	}
	name = std::string(text.substr(0, equals));
	value = std::string(text.substr(equals + 1));
	hasValue = true;
}

// Expands one module's pattern against its own directory, and returns absolute
// paths. Absolute because the graph is shared by both targets and dumped for
// programs that have no idea what its working directory was; TargetResolver
// makes them relative again where that is possible.
std::vector<fs::path> expandSources(const ModuleDef& def, const std::string& pattern,
                                    const char* what, const std::string& owner, bool quiet)
{
	Glob::Options options;
	const std::vector<fs::path> matched = Glob::expand(pattern, def.dir, options);

	if (matched.empty() && !quiet)
	{
		Log::out << OWARN << (Glob::hasWildcard(pattern)
			? "Pattern matched nothing: "
			: "Ignored non-existent path: ") << OSTR(pattern)
			<< OREASONNL "in " << what << ' ' << OSTR(owner) << std::endl;
	}

	// Normalized, because a module reaching a sibling with "../other/x.cpp"
	// would otherwise produce a different string for the same file than the
	// sibling's own pattern does, and two components claiming one file is
	// exactly what the next pass has to be able to see.
	std::vector<fs::path> out;
	out.reserve(matched.size());
	for (const fs::path& path : matched)
		out.push_back((path.is_absolute() ? path : def.dir / path).lexically_normal());
	return out;
}

std::vector<fs::path> expandIncludes(const ModuleDef& def, const std::string& pattern,
                                     const std::string& owner, bool quiet)
{
	Glob::Options options;
	options.directoriesOnly = true;
	const std::vector<fs::path> matched = Glob::expand(pattern, def.dir, options);

	if (matched.empty() && !quiet)
	{
		Log::out << OWARN << "Include directory " << OSTR(pattern) << " does not exist."
		         << OREASONNL "in module " << OSTR(owner) << std::endl;
	}

	std::vector<fs::path> out;
	out.reserve(matched.size());
	for (const fs::path& path : matched)
		out.push_back((path.is_absolute() ? path : def.dir / path).lexically_normal());
	return out;
}

void addDefine(TargetContribution& contribution, std::string_view text,
               std::string origin, Problems& problems, bool quiet)
{
	ResolvedDefine define;
	splitDefine(text, define.name, define.value, define.hasValue);
	define.origin = std::move(origin);

	if (define.name.empty())
	{
		std::ostringstream oss;
		oss << "Malformed define " << OSTR(text) << " in " << define.origin << ".";
		problems.add(oss.str());
		return;
	}

	for (ResolvedDefine& existing : contribution.defines)
	{
		if (existing.name != define.name)
			continue;

		// Two modules that disagree about a constant is not something either of
		// them can see, and the one that loses is compiled against a value its
		// own author never wrote.
		if (!quiet && (existing.value != define.value || existing.hasValue != define.hasValue))
		{
			Log::out << OWARN << "Conflicting definition of " << OSTRa(define.name) << "."
			         << OREASONNL << existing.origin << " defines it as "
			         << OSTR(existing.hasValue ? existing.value : std::string("(nothing)"))
			         << OREASONNL << define.origin << " defines it as "
			         << OSTR(define.hasValue ? define.value : std::string("(nothing)"))
			         << OREASONNL "The second wins." << std::endl;
		}
		existing = std::move(define);
		return;
	}

	contribution.defines.push_back(std::move(define));
}

void addUnique(std::vector<fs::path>& list, const fs::path& path)
{
	if (std::find(list.begin(), list.end(), path) == list.end())
		list.push_back(path);
}

} // namespace

// Declared at namespace scope rather than in an anonymous one so that it can be
// the friend ModuleGraph names.
class ModuleResolver
{
public:
	ModuleResolver(const config::ModulesConfig& config, const fs::path& workDir,
	               const ResolveOptions& options) :
		m_config(config), m_workDir(workDir), m_options(options) {}

	ModuleGraph run();

private:
	void discover();
	void validateIds();
	void applyOverrides();
	void validateRequires();
	void collectClaims();
	void fold();

	// Both halves of a `requires:` entry, resolved. Returns nullptr and leaves
	// `reason` set when it cannot be.
	[[nodiscard]] const ResolvedComponent* lookupRequirement(
		const ResolvedModule& from, std::string_view text, std::string& reason) const;

	[[nodiscard]] TargetContribution& contribution(bool arm9)
	{
		return arm9 ? m_graph.m_arm9 : m_graph.m_arm7;
	}

	const config::ModulesConfig& m_config;
	fs::path m_workDir;
	ResolveOptions m_options;

	ModuleGraph m_graph;
	Problems m_problems;

	// Index into m_graph.m_modules, by selection order. Selections whose module
	// was skipped have no entry.
	std::vector<std::size_t> m_loaded;

	struct Claim
	{
		std::size_t moduleIndex;
		std::size_t componentIndex;
		int destination;
	};

	// (absolute source path, arm9) -> the component that asked for it.
	std::map<std::pair<std::string, bool>, Claim> m_claims;

	// Files owned by a component the project switched off. They are dropped
	// from the module's catch-all patterns, which is the whole point of
	// declaring them on the component.
	std::set<std::string> m_excluded;
};

void ModuleResolver::discover()
{
	m_graph.m_directory = m_config.dir.value.is_absolute()
		? m_config.dir.value
		: m_workDir / m_config.dir.value;

	for (const config::ModuleSelection& selection : m_config.selections)
	{
		if (!selection.enabled)
		{
			// Not read at all. A module that is switched off should not be able
			// to break a build by being malformed, and nothing downstream has
			// anything to ask about it.
			ResolvedModule module;
			module.key = selection.key;
			module.name = selection.key;
			m_graph.m_modules.push_back(std::move(module));
			m_loaded.push_back(m_graph.m_modules.size() - 1);
			continue;
		}

		const fs::path dir = m_graph.m_directory / selection.key;
		const fs::path file = dir / "module.yaml";

		if (!fs::exists(file))
		{
			if (selection.optional)
			{
				if (!m_options.quiet)
				{
					Log::out << OWARN << "Optional module " << OSTR(selection.key)
					         << " is not installed, and was skipped." << std::endl;
				}
				continue;
			}

			std::ostringstream oss;
			oss << "Module " << OSTR(selection.key) << " was not found."
			    << OREASONNL "Expected " << OSTR(file.string()) << "."
			    << OREASONNL "Mark it " << OSTRa("optional: true") << " if it is meant to be absent.";
			m_problems.addAt(selection.location, oss.str());
			continue;
		}

		try {
			auto def = std::make_shared<ModuleDef>(loadModuleFile(file, selection.key));

			ResolvedModule module;
			module.key = def->key;
			module.id = def->id;
			module.name = def->name.empty() ? def->key : def->name;
			module.description = def->description;
			module.repo = def->repo;
			module.authors = def->authors;
			module.dir = def->dir;
			module.file = def->file;
			module.def = def.get();

			for (const ModuleTarget& target : def->targets)
			{
				if (!target.target.valid)
					continue;
				(target.target.arm9 ? module.targetsArm9 : module.targetsArm7) = true;
			}

			for (const ComponentDef& componentDef : def->components)
			{
				ResolvedComponent component;
				component.name = componentDef.name;
				component.target = componentDef.target;
				component.declaredTarget = componentDef.targetText;
				component.files = componentDef.files;
				component.requires_ = componentDef.requires_;
				component.def = &componentDef;
				module.components.push_back(std::move(component));
			}

			m_graph.m_defs.push_back(std::move(def));
			m_graph.m_modules.push_back(std::move(module));
			m_loaded.push_back(m_graph.m_modules.size() - 1);
		} catch (const std::exception& e) {
			m_problems.add(e.what());
		}
	}
}

void ModuleResolver::validateIds()
{
	std::unordered_map<std::string, std::string> byId;

	for (ResolvedModule& module : m_graph.m_modules)
	{
		if (module.def == nullptr)
			continue;

		const cfg::Node root = module.def->document->root();

		if (!validId(module.id))
		{
			std::ostringstream oss;
			if (module.id.empty())
				oss << "Module " << OSTR(module.key) << " has no " << OSTRa("id") << ".";
			else
				oss << "Module " << OSTR(module.key) << " has the invalid id " << OSTR(module.id) << ".";
			oss << OREASONNL "An id starts with a letter and continues with letters and digits: "
			    << "it becomes the " << OSTRa("MODULE_<ID>") << " define and the name a "
			    << OSTRa("requires") << " entry uses.";
			m_problems.add(root, oss.str());
			continue;
		}

		const auto [it, inserted] = byId.emplace(module.id, module.key);
		if (!inserted)
		{
			std::ostringstream oss;
			oss << "Modules " << OSTR(it->second) << " and " << OSTR(module.key)
			    << " both use the id " << OSTR(module.id) << "."
			    << OREASONNL "An id identifies one module.";
			m_problems.add(root, oss.str());
		}
	}
}

void ModuleResolver::applyOverrides()
{
	std::size_t index = 0;
	for (const config::ModuleSelection& selection : m_config.selections)
	{
		ResolvedModule* module = nullptr;
		for (ResolvedModule& candidate : m_graph.m_modules)
		{
			if (candidate.key == selection.key)
			{
				module = &candidate;
				break;
			}
		}
		index++;

		if (module == nullptr || module->def == nullptr)
			continue;   // missing, optional or disabled; already handled

		for (const config::ComponentOverride& override_ : selection.components)
		{
			ResolvedComponent* component = nullptr;
			for (ResolvedComponent& candidate : module->components)
			{
				if (candidate.name == override_.name)
				{
					component = &candidate;
					break;
				}
			}

			if (component == nullptr)
			{
				std::ostringstream oss;
				oss << "Module " << OSTR(module->key) << " has no component "
				    << OSTR(override_.name) << ".";
				m_problems.addAt(override_.location, oss.str());
				continue;
			}

			if (override_.hasEnabled && !override_.enabled)
			{
				component->enabled = false;
				component->disabledReason = "the project switched it off";
			}

			if (!override_.target.empty())
			{
				if (component->target.locked)
				{
					// The module said this target is not negotiable. Saying so
					// out loud is the point: the prototype swallowed the
					// override and left the project believing it had taken.
					if (!m_options.quiet)
					{
						Log::out << OWARN << "Ignoring the target override for "
						         << OSTRa(qualify(*module, *component)) << "."
						         << OREASONNL "The module locked it to "
						         << OSTR(component->target.str()) << " with a leading "
						         << OSTRa("!") << "." << std::endl;
					}
					component->targetOrigin = TargetOrigin::OverrideRefused;
				}
				else
				{
					TargetRef target;
					std::string error;
					if (!parseTargetRef(override_.target, target, error))
					{
						m_problems.addAt(override_.location, std::move(error));
					}
					else
					{
						component->target = target;
						component->targetOrigin = TargetOrigin::Project;
					}
				}
			}

			for (const auto& [name, value] : override_.defines)
			{
				bool found = false;
				for (const DefineText& declared : component->def->defines)
				{
					std::string declaredName, declaredValue;
					bool hasValue = false;
					splitDefine(declared.text, declaredName, declaredValue, hasValue);
					if (declaredName != name)
						continue;
					found = true;
					break;
				}

				if (!found)
				{
					std::ostringstream oss;
					oss << "Component " << OSTR(qualify(*module, *component))
					    << " does not define " << OSTRa(name) << ", so there is nothing to override."
					    << OREASONNL "A project can change what a component defines; it cannot "
					    << "add a define of its own here; use the target's "
					    << OSTRa("defines") << " list for that.";
					m_problems.addAt(override_.location, oss.str());
				}
			}
		}
	}
	(void)index;
}

const ResolvedComponent* ModuleResolver::lookupRequirement(
	const ResolvedModule& from, std::string_view text, std::string& reason) const
{
	std::string_view moduleName;
	std::string_view componentName = text;

	const std::size_t dot = text.find('.');
	if (dot != std::string_view::npos)
	{
		moduleName = text.substr(0, dot);
		componentName = text.substr(dot + 1);
	}

	const ResolvedModule* module = &from;
	if (!moduleName.empty())
	{
		module = m_graph.find(moduleName);
		if (module == nullptr)
		{
			std::ostringstream oss;
			oss << "no enabled module is called " << OSTR(std::string(moduleName));
			reason = oss.str();
			return nullptr;
		}
	}

	const ResolvedComponent* component = module->findComponent(componentName);
	if (component == nullptr)
	{
		std::ostringstream oss;
		oss << "module " << OSTR(module->key) << " has no component "
		    << OSTR(std::string(componentName));
		reason = oss.str();
		return nullptr;
	}

	return component;
}

void ModuleResolver::validateRequires()
{
	for (const ResolvedModule& module : m_graph.m_modules)
	{
		if (module.def == nullptr)
			continue;

		for (const ResolvedComponent& component : module.components)
		{
			if (!component.enabled)
				continue;

			for (const std::string& requirement : component.requires_)
			{
				std::string reason;
				const ResolvedComponent* required = lookupRequirement(module, requirement, reason);

				if (required == nullptr)
				{
					std::ostringstream oss;
					oss << "Component " << OSTR(qualify(module, component)) << " requires "
					    << OSTR(requirement) << ", but " << reason << ".";
					m_problems.add(module.def->document->root(), oss.str());
					continue;
				}

				if (!required->enabled)
				{
					std::ostringstream oss;
					oss << "Component " << OSTR(qualify(module, component)) << " requires "
					    << OSTR(requirement) << ", which is disabled."
					    << OREASONNL "Either enable " << OSTRa(requirement) << " or disable "
					    << OSTRa(qualify(module, component)) << " as well.";
					m_problems.add(module.def->document->root(), oss.str());
				}
			}
		}
	}
}

void ModuleResolver::collectClaims()
{
	for (std::size_t moduleIndex = 0; moduleIndex < m_graph.m_modules.size(); moduleIndex++)
	{
		ResolvedModule& module = m_graph.m_modules[moduleIndex];
		if (module.def == nullptr)
			continue;

		for (std::size_t componentIndex = 0; componentIndex < module.components.size(); componentIndex++)
		{
			ResolvedComponent& component = module.components[componentIndex];
			const std::string owner = qualify(module, component);

			std::vector<fs::path> sources;
			for (const std::string& pattern : component.def->sources)
			{
				for (const fs::path& path :
					expandSources(*module.def, pattern, "component", owner, m_options.quiet))
					addUnique(sources, path);
			}

			if (!component.enabled)
			{
				for (const fs::path& path : sources)
					m_excluded.insert(path.generic_string());
				continue;
			}

			if (sources.empty())
				continue;

			if (!component.target.valid)
			{
				// A component with sources and no target has nowhere to put
				// them, and the module's own catch-all would have swept them up
				// anyway, silently, into whichever region it names.
				std::ostringstream oss;
				oss << "Component " << OSTR(owner) << " lists sources but names no "
				    << OSTRa("target") << "." OREASONNL
				    << "Give it one, or move the sources to the module's " << OSTRa("targets") << " list.";
				m_problems.add(module.def->document->root(), oss.str());
				continue;
			}

			for (const fs::path& path : sources)
			{
				const auto key = std::make_pair(path.generic_string(), component.target.arm9);
				const auto [it, inserted] = m_claims.emplace(
					key, Claim{ moduleIndex, componentIndex, component.target.overlay });

				if (!inserted)
				{
					const ResolvedModule& other = m_graph.m_modules[it->second.moduleIndex];
					const ResolvedComponent& otherComponent = other.components[it->second.componentIndex];

					TargetRef otherTarget;
					otherTarget.valid = true;
					otherTarget.arm9 = component.target.arm9;
					otherTarget.overlay = it->second.destination;
					const std::string otherRegion = otherTarget.regionName();

					std::ostringstream oss;
					oss << "Two components both claim " << OSTR(path.generic_string()) << "."
					    << OREASONNL << OSTRa(qualify(other, otherComponent)) << " puts it in "
					    << OSTR(otherRegion)
					    << OREASONNL << OSTRa(owner) << " puts it in "
					    << OSTR(component.target.regionName())
					    << OREASONNL "A source file belongs to one component.";
					m_problems.add(module.def->document->root(), oss.str());
					continue;
				}

				component.sources.push_back(path);
			}
		}
	}
}

void ModuleResolver::fold()
{
	for (std::size_t moduleIndex = 0; moduleIndex < m_graph.m_modules.size(); moduleIndex++)
	{
		ResolvedModule& module = m_graph.m_modules[moduleIndex];
		if (module.def == nullptr)
			continue;

		// MODULE_<ID> first, so that a module define of the same name is the one
		// that stands, since a module is allowed to say what its own flag means.
		const std::string moduleDefine = "MODULE_" + upper(module.id);
		std::ostringstream originStream;
		originStream << "module " << module.key;
		const std::string origin = originStream.str();

		bool arm9 = module.targetsArm9;
		bool arm7 = module.targetsArm7;
		for (const ResolvedComponent& component : module.components)
		{
			if (component.enabled && component.target.valid)
				(component.target.arm9 ? arm9 : arm7) = true;
		}

		// A module that names no target at all still reaches the main processor:
		// a defines-only module is a real thing, and arm9 is where code goes
		// unless somebody says otherwise.
		if (!arm9 && !arm7)
			arm9 = true;

		module.targetsArm9 = arm9;
		module.targetsArm7 = arm7;

		for (bool proc : { false, true })
		{
			if (proc ? !arm9 : !arm7)
				continue;
			addDefine(contribution(proc), moduleDefine, origin, m_problems, m_options.quiet);
			for (const DefineText& define : module.def->defines)
				addDefine(contribution(proc), define.text, origin, m_problems, m_options.quiet);
		}

		// Catch-alls: includes, then whatever the patterns sweep up that no
		// component has spoken for.
		for (const ModuleTarget& target : module.def->targets)
		{
			if (!target.target.valid)
				continue;

			TargetContribution& into = contribution(target.target.arm9);

			for (const std::string& pattern : target.includes)
			{
				for (const fs::path& path : expandIncludes(*module.def, pattern, module.key, m_options.quiet))
					addUnique(into.includes, path);
			}

			for (const std::string& pattern : target.sources)
			{
				for (const fs::path& path :
					expandSources(*module.def, pattern, "module", module.key, m_options.quiet))
				{
					const std::string generic = path.generic_string();
					if (m_excluded.count(generic) != 0)
						continue;
					if (m_claims.count(std::make_pair(generic, target.target.arm9)) != 0)
						continue;
					addUnique(into.regionSources[target.target.overlay], path);
				}
			}
		}

		// Components: their defines, their includes, and the sources they claimed.
		for (const ResolvedComponent& component : module.components)
		{
			if (!component.enabled)
				continue;

			// A component that names no target is a switch and nothing else,
			// and it belongs wherever its module does. The prototype dropped it
			// on the floor instead, which is why nwav's HasEvents and HasStereo
			// have never once reached a compiler.
			std::vector<bool> procs;
			if (component.target.valid)
			{
				procs.push_back(component.target.arm9);
			}
			else
			{
				if (arm9) procs.push_back(true);
				if (arm7) procs.push_back(false);
			}

			const std::string owner = "component " + qualify(module, component);

			for (const bool proc : procs)
			{
				TargetContribution& into = contribution(proc);

				for (const DefineText& define : component.def->defines)
				{
					std::string name, value;
					bool hasValue = false;
					splitDefine(define.text, name, value, hasValue);

					// A project override replaces the value and nothing else.
					std::string text = define.text;
					for (const config::ModuleSelection& selection : m_config.selections)
					{
						if (selection.key != module.key)
							continue;
						for (const config::ComponentOverride& override_ : selection.components)
						{
							if (override_.name != component.name)
								continue;
							for (const auto& [overrideName, overrideValue] : override_.defines)
							{
								if (overrideName == name)
									text = name + "=" + overrideValue;
							}
						}
					}

					addDefine(into, text, owner, m_problems, m_options.quiet);
				}

				for (const std::string& pattern : component.def->includes)
				{
					for (const fs::path& path : expandIncludes(*module.def, pattern, module.key, m_options.quiet))
						addUnique(into.includes, path);
				}

				if (component.target.valid)
				{
					for (const fs::path& path : component.sources)
						addUnique(into.regionSources[component.target.overlay], path);
				}
			}
		}

		// Every component keeps its resolved defines, so that `modules explain`
		// and the dump can attribute them.
		for (ResolvedComponent& component : module.components)
		{
			for (const DefineText& define : component.def->defines)
			{
				ResolvedDefine resolved;
				splitDefine(define.text, resolved.name, resolved.value, resolved.hasValue);
				resolved.origin = "component " + qualify(module, component);
				component.defines.push_back(std::move(resolved));
			}
		}
	}
}

ModuleGraph ModuleResolver::run()
{
	m_graph.m_configured = m_config.present;
	if (!m_config.present)
		return std::move(m_graph);

	discover();
	validateIds();
	applyOverrides();
	validateRequires();

	// Nothing below this point can produce a coherent answer if the graph is
	// already wrong: a claim conflict between two modules, one of which failed
	// to load, would be reported against a module that is not there.
	m_problems.raise("The module configuration could not be resolved.");

	collectClaims();
	fold();

	m_problems.raise("The module configuration could not be resolved.");

	return std::move(m_graph);
}

ModuleGraph resolve(const config::ModulesConfig& config, const fs::path& workDir,
                    const ResolveOptions& options)
{
	ModuleResolver resolver(config, workDir, options);
	return resolver.run();
}

} // namespace ncp::modules
