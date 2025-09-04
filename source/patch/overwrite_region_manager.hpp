#pragma once

#include <vector>
#include <memory>
#include <unordered_map>

#include "../utils/types.hpp"
#include "../formats/elf.hpp"
#include "../config/buildtarget.hpp"
#include "dependency_resolver.hpp"
#include "types.hpp"

namespace ncp::patch {

class OverwriteRegionManager
{
public:
    OverwriteRegionManager();
    ~OverwriteRegionManager();

    void initialize(const BuildTarget& target, const DependencyResolver& dependencyResolver);
    
    void setupOverwriteRegions();
    void assignSectionsToOverwrites(std::vector<std::unique_ptr<SectionInfo>>& candidateSections);

	void checkForConflictsWithPatches(const std::vector<std::unique_ptr<PatchInfo>>& patches);
	void finalizeOverwritesWithElfData(const Elf32& elf);

    const std::vector<std::unique_ptr<OverwriteRegionInfo>>& getOverwriteRegions() const { return m_overwriteRegions; }
    std::vector<std::unique_ptr<OverwriteRegionInfo>>& getOverwriteRegions() { return m_overwriteRegions; }

private:
    const BuildTarget* m_target;
	const DependencyResolver* m_dependencyResolver;
    std::vector<std::unique_ptr<OverwriteRegionInfo>> m_overwriteRegions;
};

}
