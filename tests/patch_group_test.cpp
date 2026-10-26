// Checks the group table sizes the linker script reserves.

#include "../source/patch/patch_group.hpp"

#include <iostream>

using namespace ncp::patch;

static std::unique_ptr<PatchInfo> makePatch(PatchType type, const char* group, bool destThumb = false, u32 sectionSize = 0)
{
	auto info = std::make_unique<PatchInfo>();
	info->type = type;
	info->group = group;
	info->destThumb = destThumb;
	info->sectionSize = sectionSize;
	return info;
}

int main()
{
	bool passed = true;
	auto check = [&passed](bool condition, const std::string& message) {
		if (!condition)
		{
			std::cout << "FAIL: " << message << '\n';
			passed = false;
		}
	};

	std::vector<std::unique_ptr<PatchInfo>> patches;
	patches.push_back(makePatch(PatchType::Call, "mvsl"));
	patches.push_back(makePatch(PatchType::Jump, ""));
	patches.push_back(makePatch(PatchType::Over, "a_group", false, 6));
	patches.push_back(makePatch(PatchType::Jump, "mvsl", true));
	patches.push_back(makePatch(PatchType::Hook, "mvsl"));

	std::vector<GroupLayout> groups = layoutGroups(patches);
	check(groups.size() == 2, "two groups");
	if (groups.size() == 2)
	{
		// Ordered by name, build-time patches left out.
		check(groups[0].name == "a_group", "first group is a_group");
		check(groups[0].symbol() == "ncprt_group_a_group", "symbol name");
		check(groups[0].patches.size() == 1, "a_group has one patch");
		check(groups[0].size == 8 + 16 + 8, "a_group saves 6 bytes, padded to 8");

		check(groups[1].name == "mvsl", "second group is mvsl");
		check(groups[1].patches.size() == 3, "mvsl has three patches");
		check(groups[1].patches[0] == patches[0].get() && groups[1].patches[2] == patches[4].get(), "mvsl keeps patch order");
		check(groups[1].size == 8 + 3 * 16 + 4 + 8 + 4, "mvsl saves a call, a THUMB jump and a hook");
	}

	check(layoutGroups({}).empty(), "no patches, no groups");

	if (passed)
		std::cout << "all checks passed\n";
	return passed ? 0 : 1;
}
