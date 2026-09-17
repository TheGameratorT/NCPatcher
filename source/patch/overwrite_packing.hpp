#pragma once

#include <cstddef>
#include <vector>

#include "../utils/types.hpp"

namespace ncp::patch {

// One candidate section, with its real (measured) size and alignment. `size`
// of 0 means the section did not survive linking and is not a candidate at
// all.
struct PackItem
{
	u32 size;
	u32 alignment;
	int destination;
};

// One overwrite region. Items are only placed into regions sharing their
// destination.
struct PackRegion
{
	u32 startAddress;
	u32 endAddress;
	int destination;
};

// One item placed into one region, at an absolute address.
struct PackPlacement
{
	std::size_t itemIndex;
	std::size_t regionIndex;
	u32 address;
};

struct PackResult
{
	// In per-region emission order: every placement for regionIndex 0 first
	// (in the order they should be written to the linker script), then
	// regionIndex 1, and so on.
	std::vector<PackPlacement> placements;

	// One entry per region passed in, parallel to `regions`.
	std::vector<u32> usedSize;

	// Indices into `items` that did not fit in any region of their
	// destination. Items with `size == 0` are never counted here: they never
	// existed as far as packing is concerned.
	std::vector<std::size_t> spilled;

	u32 spilledBytes;
};

// Packs measured sections into overwrite regions.
//
// Within a region, items are ordered by descending alignment, then
// descending size, then ascending original index (for stable output).
// Ordering by alignment first is what removes inter-item padding: an item
// that does not need 8-byte alignment should never be the reason a later
// 8-byte item pays for realignment.
//
// Selection is first-fit-decreasing by size: items are tried largest first,
// against that destination's regions in order of most free space first.
//
// The packing cursor is an absolute address starting at `region.startAddress`,
// not an offset, because alignment applies to the absolute address: a region
// that does not start on an 8-byte boundary changes how an 8-byte item pads.
PackResult packOverwriteRegions(
	const std::vector<PackRegion>& regions,
	const std::vector<PackItem>& items
);

} // namespace ncp::patch
