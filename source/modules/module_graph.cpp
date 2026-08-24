#include "module_graph.hpp"

namespace ncp::modules {

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
