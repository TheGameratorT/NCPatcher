#pragma once

#include <vector>
#include <memory>
#include <unordered_map>

#include "../utils/types.hpp"
#include "../formats/elf.hpp"
#include "../config/buildtarget.hpp"
#include "../app/context.hpp"
#include "types.hpp"

namespace ncp::patch {

// What assignMeasuredSections() found out about how full the overwrite
// regions ended up, for the "Patching" milestone to report. Distinct from the
// per-region detail it prints under --verbose-tag: this is the one-line total
// a default build gets instead.
struct OverwriteStats
{
    u64 usedBytes = 0;
    u64 capacityBytes = 0;
    u64 spilledBytes = 0;
    std::size_t spilledSections = 0;
};

class OverwriteRegionManager
{
public:
    OverwriteRegionManager();
    ~OverwriteRegionManager();

    void initialize(const ncp::Context& ctx, const BuildTarget& target);
    
    void setupOverwriteRegions();

    // Assigns candidate sections to overwrite regions using their real,
    // post-link sizes (measured by a throwaway linker pass), not a guess at
    // what the linker will do.
    void assignMeasuredSections(
        const std::vector<std::unique_ptr<SectionInfo>>& candidateSections,
        const std::vector<u32>& measuredSizes
    );

	void checkForConflictsWithPatches(const std::vector<std::unique_ptr<PatchInfo>>& patches);
	void finalizeOverwritesWithElfData(const Elf32& elf);

    const std::vector<std::unique_ptr<OverwriteRegionInfo>>& getOverwriteRegions() const { return m_overwriteRegions; }
    std::vector<std::unique_ptr<OverwriteRegionInfo>>& getOverwriteRegions() { return m_overwriteRegions; }

    const OverwriteStats& getStats() const { return m_stats; }

private:
    const ncp::Context* m_ctx;
    const BuildTarget* m_target;
    std::vector<std::unique_ptr<OverwriteRegionInfo>> m_overwriteRegions;
    OverwriteStats m_stats;
};

}
