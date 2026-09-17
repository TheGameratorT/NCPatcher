#include "../source/patch/overwrite_packing.hpp"
#include "../source/rom/layout.hpp"

#include <iostream>

using namespace ncp::patch;

int main()
{
	bool passed = true;
	auto check = [&passed](bool condition, const char* message) {
		if (!condition)
		{
			std::cout << "FAIL: " << message << '\n';
			passed = false;
		}
	};

	// Exact padding across mixed alignments: a region starting at an 8-aligned
	// address holding a 4-byte item then an 8-byte item should pad the 4-byte
	// item's end up to 8, not further.
	{
		std::vector<PackRegion> regions = { { 0x1000, 0x1100, 0 } };
		std::vector<PackItem> items = {
			{ 4, 4, 0 }, // ends up first (larger alignment sorts first, tie broken by size then index)
			{ 8, 8, 0 },
		};
		PackResult r = packOverwriteRegions(regions, items);
		check(r.spilled.empty(), "small items should not spill");
		check(r.placements.size() == 2, "both items should be placed");
		// Descending alignment first: the 8-aligned item goes first.
		check(r.placements[0].itemIndex == 1, "higher alignment item should be placed first");
		check(r.placements[0].address == 0x1000, "first item should start at region start");
		check(r.placements[1].itemIndex == 0, "lower alignment item should be placed second");
		check(r.placements[1].address == 0x1008, "second item should follow the first with no gap");
		check(r.usedSize[0] == 12, "used size should be exactly item sizes with no extra padding");
	}

	// Descending alignment then descending size ordering, ignoring insertion
	// order.
	{
		std::vector<PackRegion> regions = { { 0x2000, 0x3000, 0 } };
		std::vector<PackItem> items = {
			{ 10, 4, 0 },
			{ 20, 4, 0 },
			{ 30, 8, 0 },
		};
		PackResult r = packOverwriteRegions(regions, items);
		check(r.placements.size() == 3, "all three items should be placed");
		check(r.placements[0].itemIndex == 2, "the 8-aligned item should be placed first");
		check(r.placements[1].itemIndex == 1, "of equal alignment, the larger item should be placed next");
		check(r.placements[2].itemIndex == 0, "the smallest item should be placed last");
	}

	// size == 0 is not a candidate: not placed, not spilled.
	{
		std::vector<PackRegion> regions = { { 0x1000, 0x1010, 0 } };
		std::vector<PackItem> items = { { 0, 4, 0 } };
		PackResult r = packOverwriteRegions(regions, items);
		check(r.placements.empty(), "a zero-size item should not be placed");
		check(r.spilled.empty(), "a zero-size item should not be counted as spilled");
		check(r.usedSize[0] == 0, "a region with nothing placed should report zero used size");
	}

	// Spill when a region is too small.
	{
		std::vector<PackRegion> regions = { { 0x1000, 0x1008, 0 } };
		std::vector<PackItem> items = { { 4, 4, 0 }, { 100, 4, 0 } };
		PackResult r = packOverwriteRegions(regions, items);
		check(r.placements.size() == 1, "only the item that fits should be placed");
		check(r.spilled.size() == 1 && r.spilled[0] == 1, "the oversized item should spill");
		check(r.spilledBytes == 100, "spilled bytes should equal the spilled item's size");
	}

	// Two equal-capacity regions for one destination: once the first fills up,
	// the next item should spill over into the second rather than being
	// dropped, since ties in free space break toward region order.
	{
		std::vector<PackRegion> regions = {
			{ 0x1000, 0x1004, 0 }, // 4 bytes free
			{ 0x2000, 0x2004, 0 }, // 4 bytes free
		};
		std::vector<PackItem> items = { { 4, 4, 0 }, { 4, 4, 0 } };
		PackResult r = packOverwriteRegions(regions, items);
		check(r.spilled.empty(), "both small items should fit somewhere");
		check(r.placements.size() == 2, "both items should be placed");
		check(r.placements[0].regionIndex == 0, "the first item should fill the first region");
		check(r.placements[1].regionIndex == 1, "the second item should spill into the second region");
	}

	// Two unequal regions for one destination: an item that fits in either
	// should go to whichever currently has the most free space, so both end
	// up packed into the larger region rather than spread arbitrarily.
	{
		std::vector<PackRegion> regions = {
			{ 0x1000, 0x1004, 0 }, // 4 bytes free
			{ 0x2000, 0x2100, 0 }, // 256 bytes free
		};
		std::vector<PackItem> items = { { 4, 4, 0 }, { 4, 4, 0 } };
		PackResult r = packOverwriteRegions(regions, items);
		check(r.spilled.empty(), "both small items should fit somewhere");
		check(r.placements.size() == 2, "both items should be placed");
		check(r.placements[0].regionIndex == 1 && r.placements[1].regionIndex == 1,
			"both items should pack into the larger region rather than spread");
	}

	// An item whose destination has no matching region spills entirely.
	{
		std::vector<PackRegion> regions = { { 0x1000, 0x2000, 0 } };
		std::vector<PackItem> items = { { 4, 4, 1 } };
		PackResult r = packOverwriteRegions(regions, items);
		check(r.placements.empty(), "an item with no matching region should not be placed");
		check(r.spilled.size() == 1, "an item with no matching region should spill");
	}

	// A region whose start address is only 4-aligned, holding an item that
	// needs 8-byte alignment: the first placement must round up from the
	// region start, not assume it is already 8-aligned.
	{
		std::vector<PackRegion> regions = { { 0x1004, 0x2000, 0 } };
		std::vector<PackItem> items = { { 8, 8, 0 } };
		PackResult r = packOverwriteRegions(regions, items);
		check(r.placements.size() == 1, "the item should be placed");
		check(r.placements[0].address == 0x1008, "placement should align up from an unaligned region start");
		check(r.usedSize[0] == ncp::rom::alignUp(0x1008 + 8 - 0x1004, 4), "used size should include the leading pad");
	}

	// A last-resort item is placed after a normal item even when it would
	// have sorted first: higher alignment and greater size do not let a
	// last-resort item cut ahead of a normal one.
	{
		std::vector<PackRegion> regions = { { 0x1000, 0x1100, 0 } };
		std::vector<PackItem> items = {
			{ 4, 4, 0, /*lastResort=*/true },
			{ 8, 8, 0, /*lastResort=*/false },
		};
		PackResult r = packOverwriteRegions(regions, items);
		check(r.spilled.empty(), "both items should fit");
		check(r.placements.size() == 2, "both items should be placed");
		check(r.placements[0].itemIndex == 1, "the normal item should be placed first despite lower alignment/size");
		check(r.placements[0].address == 0x1000, "the normal item should start at the region start");
		check(r.placements[1].itemIndex == 0, "the last-resort item should be placed second");
	}

	// When a region only has room for one of two items, the normal item
	// wins the space and the last-resort item spills, even though it was
	// enqueued first.
	{
		std::vector<PackRegion> regions = { { 0x1000, 0x1004, 0 } };
		std::vector<PackItem> items = {
			{ 4, 4, 0, /*lastResort=*/true },
			{ 4, 4, 0, /*lastResort=*/false },
		};
		PackResult r = packOverwriteRegions(regions, items);
		check(r.placements.size() == 1 && r.placements[0].itemIndex == 1, "the normal item should take the space");
		check(r.spilled.size() == 1 && r.spilled[0] == 0, "the last-resort item should spill instead");
		check(r.spilledBytes == 4, "spilled bytes should equal the spilled item's size");
	}

	// A normal item still goes to the region with the most free space even
	// with a last-resort item present, so pass order does not pre-empt the
	// free-space try order.
	{
		std::vector<PackRegion> regions = {
			{ 0x1000, 0x1004, 0 }, // 4 bytes free
			{ 0x2000, 0x2100, 0 }, // 256 bytes free
		};
		std::vector<PackItem> items = {
			{ 4, 4, 0, /*lastResort=*/true },
			{ 4, 4, 0, /*lastResort=*/false },
		};
		PackResult r = packOverwriteRegions(regions, items);
		check(r.spilled.empty(), "both items should fit somewhere");
		std::size_t normalRegion = (r.placements[0].itemIndex == 1) ? r.placements[0].regionIndex : r.placements[1].regionIndex;
		check(normalRegion == 1, "the normal item should still go to the region with more free space");
	}

	// Within the last-resort pass, ordering still follows descending
	// alignment then descending size.
	{
		std::vector<PackRegion> regions = { { 0x2000, 0x3000, 0 } };
		std::vector<PackItem> items = {
			{ 10, 4, 0, /*lastResort=*/true },
			{ 20, 4, 0, /*lastResort=*/true },
			{ 30, 8, 0, /*lastResort=*/true },
		};
		PackResult r = packOverwriteRegions(regions, items);
		check(r.placements.size() == 3, "all three last-resort items should be placed");
		check(r.placements[0].itemIndex == 2, "the 8-aligned item should be placed first");
		check(r.placements[1].itemIndex == 1, "of equal alignment, the larger item should be placed next");
		check(r.placements[2].itemIndex == 0, "the smallest item should be placed last");
	}

	// A region holding only last-resort items still reports the correct
	// used size.
	{
		std::vector<PackRegion> regions = { { 0x1000, 0x1100, 0 } };
		std::vector<PackItem> items = { { 12, 4, 0, /*lastResort=*/true } };
		PackResult r = packOverwriteRegions(regions, items);
		check(r.placements.size() == 1, "the last-resort item should be placed");
		check(r.usedSize[0] == 12, "used size should equal the placed item's size");
	}

	if (passed)
		std::cout << "overwrite_packing_test: all checks passed\n";
	return passed ? 0 : 1;
}
