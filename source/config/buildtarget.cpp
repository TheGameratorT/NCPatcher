#include "buildtarget.hpp"

BuildTarget::BuildTarget() = default;

bool BuildTarget::hasOverwrites() const
{
	for (const auto& region : regions)
	{
		if (!region.overwrites.empty())
			return true;
	}
	return false;
}

const BuildTarget::Region* BuildTarget::getRegionByDestination(int destination) const
{
	for (const auto& region : regions)
	{
		if (region.destination == destination)
			return &region;
	}
	return nullptr;
}

BuildTarget::Region* BuildTarget::getRegionByDestination(int destination)
{
	for (auto& region : regions)
	{
		if (region.destination == destination)
			return &region;
	}
	return nullptr;
}

const BuildTarget::Region* BuildTarget::getMainRegion() const
{
	return getRegionByDestination(-1);
}

BuildTarget::Region* BuildTarget::getMainRegion()
{
	return getRegionByDestination(-1);
}
