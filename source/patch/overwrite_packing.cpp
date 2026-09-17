#include "overwrite_packing.hpp"

#include <algorithm>
#include <unordered_map>

#include "../rom/layout.hpp"

namespace ncp::patch {

PackResult packOverwriteRegions(
	const std::vector<PackRegion>& regions,
	const std::vector<PackItem>& items
)
{
	PackResult result;
	result.usedSize.assign(regions.size(), 0);
	result.spilledBytes = 0;

	// Group region indices by destination.
	std::unordered_map<int, std::vector<std::size_t>> regionsByDest;
	for (std::size_t r = 0; r < regions.size(); r++)
		regionsByDest[regions[r].destination].push_back(r);

	// A region's cursor, tracked as an absolute address so alignment applies
	// against the real region start, not an offset from it.
	std::vector<u32> cursor(regions.size());
	for (std::size_t r = 0; r < regions.size(); r++)
		cursor[r] = regions[r].startAddress;

	std::vector<bool> regionUsed(regions.size(), false);

	// Per-region placement lists, in the order items are placed, concatenated
	// into the final result at the end.
	std::vector<std::vector<PackPlacement>> placementsByRegion(regions.size());

	// Group candidate item indices by destination, skipping anything that did
	// not survive linking (size == 0).
	std::unordered_map<int, std::vector<std::size_t>> itemsByDest;
	for (std::size_t i = 0; i < items.size(); i++)
	{
		if (items[i].size == 0)
			continue;
		itemsByDest[items[i].destination].push_back(i);
	}

	for (auto& [dest, itemIndices] : itemsByDest)
	{
		auto regionIt = regionsByDest.find(dest);
		if (regionIt == regionsByDest.end())
			continue; // No overwrite region for this destination; spills below.

		std::vector<std::size_t>& destRegions = regionIt->second;

		// Descending alignment, then descending size, then ascending original
		// index for stable output.
		std::sort(itemIndices.begin(), itemIndices.end(), [&items](std::size_t a, std::size_t b) {
			if (items[a].alignment != items[b].alignment)
				return items[a].alignment > items[b].alignment;
			if (items[a].size != items[b].size)
				return items[a].size > items[b].size;
			return a < b;
		});

		for (std::size_t itemIdx : itemIndices)
		{
			const PackItem& item = items[itemIdx];

			// Try regions of this destination in order of most free space
			// first, recomputed on every item since placement shrinks it.
			// A stable sort keeps ties in region order, so a region that
			// fills up is not revisited ahead of an equally-free one that
			// simply comes later in the config.
			std::vector<std::size_t> tryOrder = destRegions;
			std::stable_sort(tryOrder.begin(), tryOrder.end(), [&](std::size_t a, std::size_t b) {
				u32 freeA = regions[a].endAddress - cursor[a];
				u32 freeB = regions[b].endAddress - cursor[b];
				return freeA > freeB;
			});

			bool placed = false;
			for (std::size_t r : tryOrder)
			{
				u32 address = rom::alignUp(cursor[r], item.alignment);
				u32 end = address + item.size;
				if (end > regions[r].endAddress)
					continue;

				cursor[r] = end;
				regionUsed[r] = true;
				placementsByRegion[r].push_back({ itemIdx, r, address });
				placed = true;
				break;
			}

			if (!placed)
			{
				result.spilled.push_back(itemIdx);
				result.spilledBytes += item.size;
			}
		}
	}

	// Items whose destination has no overwrite region at all also spill.
	for (auto& [dest, itemIndices] : itemsByDest)
	{
		if (regionsByDest.find(dest) != regionsByDest.end())
			continue;
		for (std::size_t itemIdx : itemIndices)
		{
			result.spilled.push_back(itemIdx);
			result.spilledBytes += items[itemIdx].size;
		}
	}

	for (std::size_t r = 0; r < regions.size(); r++)
	{
		if (regionUsed[r])
			result.usedSize[r] = rom::alignUp(cursor[r] - regions[r].startAddress, 4);

		for (const PackPlacement& p : placementsByRegion[r])
			result.placements.push_back(p);
	}

	return result;
}

} // namespace ncp::patch
