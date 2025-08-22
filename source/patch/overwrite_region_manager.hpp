#pragma once

#include <vector>
#include <memory>
#include <unordered_map>

#include "../types.hpp"
#include "../config/buildtarget.hpp"
#include "patch_info_analyzer.hpp"

struct OverwriteRegionInfo
{
    u32 startAddress;
    u32 endAddress;
    int destination;
    std::vector<SectionInfo*> assignedSections;
    std::vector<GenericPatchInfo*> sectionPatches;
    u32 usedSize;
    std::string memName;
    int sectionIdx;
    int sectionSize;
};

class OverwriteRegionManager
{
public:
    OverwriteRegionManager();
    ~OverwriteRegionManager();

    void initialize(const BuildTarget& target);
    
    void setupOverwriteRegions();
    void assignSectionsToOverwrites(std::vector<std::unique_ptr<SectionInfo>>& candidateSections);

    const std::vector<std::unique_ptr<OverwriteRegionInfo>>& getOverwriteRegions() const { return m_overwriteRegions; }
    std::vector<std::unique_ptr<OverwriteRegionInfo>>& getOverwriteRegions() { return m_overwriteRegions; }

private:
    const BuildTarget* m_target;
    std::vector<std::unique_ptr<OverwriteRegionInfo>> m_overwriteRegions;
};
