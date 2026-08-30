#pragma once

// The resolved module graph: what the enabled modules amount to, once the
// project has had its say.
//
// This is not a BuildTarget and deliberately so. The dump needs the graph
// (which module a source file came from, which component a define belongs to,
// why a component is disabled) and a BuildTarget has thrown all of that away
// by construction. `modules dump` also has to work with no toolchain and no
// ROM present, which resolving a target does not.
//
// TargetResolver folds the per-processor contribution below into each target
// afterwards; everything else here exists to answer questions about the graph.

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "module_config.hpp"

namespace ncp::modules {

struct ResolvedDefine
{
	std::string name;
	std::string value;
	bool hasValue = false;

	// "module coop", "component Coop.SpikeBassFix", what the conflict warning
	// names, and what `modules explain` prints.
	std::string origin;
};

// Where a component's target came from, once the module's declaration and the
// project's override have been reconciled.
enum class TargetOrigin
{
	Module,        // the module said so and nobody argued
	Project,       // the project overrode it
	OverrideRefused // the project tried, but the module locked the target
};

struct ResolvedComponent
{
	std::string name;
	bool enabled = true;

	// Empty when enabled. Otherwise a phrase completing "disabled because ...",
	// which is the whole of what `modules explain` has to say about it.
	std::string disabledReason;

	TargetRef target;
	TargetOrigin targetOrigin = TargetOrigin::Module;
	std::string declaredTarget;   // what the module wrote, lock marker included

	std::vector<ResolvedDefine> defines;

	// Absolute, sorted, and already stripped of anything a disabled component
	// owns. TargetResolver makes them relative to the target's working
	// directory where it can, so object paths stay legible.
	std::vector<std::filesystem::path> sources;

	// Passed through untouched: which file in a game's filesystem a name refers
	// to is not something this program can know. See W6.
	std::vector<std::string> files;

	std::vector<std::string> requires_;

	const ComponentDef* def = nullptr;
};

struct ResolvedModule
{
	std::string key;
	std::string id;
	std::string name;
	std::string description;
	std::string repo;
	std::vector<std::string> authors;

	std::filesystem::path dir;
	std::filesystem::path file;

	std::vector<ResolvedComponent> components;

	// Which processors this module reaches, which is what decides where its
	// MODULE_<ID> define and its module-level defines land.
	bool targetsArm9 = false;
	bool targetsArm7 = false;

	const ModuleDef* def = nullptr;

	[[nodiscard]] const ResolvedComponent* findComponent(std::string_view name) const;
};

// Everything the modules contribute to one processor.
struct TargetContribution
{
	// In the order the project enabled the modules, so that a build's include
	// search order is a property of the configuration and not of a hash table.
	std::vector<std::filesystem::path> includes;

	std::vector<ResolvedDefine> defines;

	// Region destination (-1 for the main binary, otherwise the overlay id)
	// to the sources that land in it. Ordered, so region creation is too.
	std::map<int, std::vector<std::filesystem::path>> regionSources;

	[[nodiscard]] bool empty() const
	{
		return includes.empty() && defines.empty() && regionSources.empty();
	}
};

class ModuleGraph
{
public:
	[[nodiscard]] bool empty() const { return m_modules.empty(); }

	[[nodiscard]] const std::vector<ResolvedModule>& modules() const { return m_modules; }
	[[nodiscard]] const TargetContribution& contribution(bool arm9) const
	{
		return arm9 ? m_arm9 : m_arm7;
	}

	// By directory key or by id, since a project writes the one and the dump
	// prints the other.
	[[nodiscard]] const ResolvedModule* find(std::string_view keyOrId) const;

	// True when the project asked for modules at all, as opposed to enabling
	// none. The difference decides whether empty regions are pruned.
	[[nodiscard]] bool configured() const { return m_configured; }

	// The module directories, kept so `modules list` can print them and so the
	// dump can name them.
	[[nodiscard]] const std::filesystem::path& directory() const { return m_directory; }

private:
	friend class ModuleResolver;

	// The parsed files, owned here because the resolved graph points into them
	// and outlives the resolver that read them.
	std::vector<std::shared_ptr<ModuleDef>> m_defs;

	std::vector<ResolvedModule> m_modules;
	TargetContribution m_arm7;
	TargetContribution m_arm9;
	std::filesystem::path m_directory;
	bool m_configured = false;
};

} // namespace ncp::modules
