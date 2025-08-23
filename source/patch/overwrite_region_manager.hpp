#pragma once

#include <vector>
#include <memory>
#include <unordered_map>

#include "../types.hpp"
#include "../config/buildtarget.hpp"
#include "types.hpp"

// Use the centralized types from patch::types
using patch::SectionInfo;
using patch::GenericPatchInfo;
using patch::OverwriteRegionInfo;

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
