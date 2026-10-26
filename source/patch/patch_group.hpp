#pragma once

#include <memory>
#include <string>
#include <vector>

#include "../utils/types.hpp"
#include "types.hpp"

namespace ncp::patch {

// The ncprt_patch kinds, as ncp.h numbers them.
enum class RuntimeKind : u8 {
	Copy = 0,
	ArmB = 1,
	ArmBL = 2,
	ArmBLX = 3,
	ArmJump = 4,
	ThumbBL = 5,
	ThumbBLX = 6,
	ThumbJump = 7,
	ThumbJumpX = 8
};

// Sizes of ncprt_group and ncprt_patch.
constexpr u32 SizeOfGroupHeader = 8;
constexpr u32 SizeOfGroupEntry = 16;

// A group's block in the main binary: the ncprt_group header, one ncprt_patch
// per patch, then the bytes each patch saves, in the same order.
struct GroupLayout
{
	std::string name;
	std::vector<const PatchInfo*> patches;
	u32 size;

	std::string symbol() const { return "ncprt_group_" + name; }
};

// The groups used by `patchInfo`, ordered by name, each with its patches in
// the order they were collected.
std::vector<GroupLayout> layoutGroups(const std::vector<std::unique_ptr<PatchInfo>>& patchInfo);

} // namespace ncp::patch
