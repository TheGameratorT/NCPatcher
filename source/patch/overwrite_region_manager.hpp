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

private:
    const ncp::Context* m_ctx;
    const BuildTarget* m_target;
    std::vector<std::unique_ptr<OverwriteRegionInfo>> m_overwriteRegions;
};

}
