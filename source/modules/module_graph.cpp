#include "module_graph.hpp"

#include <algorithm>

namespace ncp::modules {

void TargetContribution::addSource(const TargetRef& target, const std::filesystem::path& path)
{
	std::vector<std::filesystem::path>& list = target.isRuntime()
		? runtime[target.runtime].sources
		: regionSources[target.overlay];
	if (std::find(list.begin(), list.end(), path) == list.end())
		list.push_back(path);
}

const ResolvedComponent* ResolvedModule::findComponent(std::string_view name) const
{
	for (const ResolvedComponent& component : components)
	{
		if (component.name == name)
			return &component;
	}
	return nullptr;
}

const ResolvedModule* ModuleGraph::find(std::string_view keyOrId) const
{
	// The directory name first: that is what a project writes, so that is what
	// a project's mistake is most likely to be about.
	for (const ResolvedModule& module : m_modules)
	{
		if (module.key == keyOrId)
			return &module;
	}
	for (const ResolvedModule& module : m_modules)
	{
		if (!module.id.empty() && module.id == keyOrId)
			return &module;
	}
	return nullptr;
}

} // namespace ncp::modules
