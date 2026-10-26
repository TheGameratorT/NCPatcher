#include "patch_group.hpp"

#include <map>

namespace ncp::patch {

std::vector<GroupLayout> layoutGroups(const std::vector<std::unique_ptr<PatchInfo>>& patchInfo)
{
	std::map<std::string, GroupLayout> groups;
	for (const auto& info : patchInfo)
	{
		if (info->group.empty())
			continue;
		GroupLayout& layout = groups[info->group];
		layout.name = info->group;
		layout.patches.push_back(info.get());
	}

	std::vector<GroupLayout> result;
	result.reserve(groups.size());
	for (auto& [name, layout] : groups)
	{
		u32 size = SizeOfGroupHeader + u32(layout.patches.size()) * SizeOfGroupEntry;
		for (const PatchInfo* p : layout.patches)
			size += p->getOverwriteAmount();
		layout.size = (size + 3) & ~3u;
		result.push_back(std::move(layout));
	}
	return result;
}

} // namespace ncp::patch
