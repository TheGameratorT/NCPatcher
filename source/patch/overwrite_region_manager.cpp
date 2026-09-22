#include "overwrite_region_manager.hpp"

#include <iomanip>
#include <unordered_map>
#include <unordered_set>

#include "../system/log.hpp"
#include "../system/except.hpp"
#include "../utils/util.hpp"
#include "overwrite_packing.hpp"

namespace ncp::patch {

OverwriteRegionManager::OverwriteRegionManager() = default;
OverwriteRegionManager::~OverwriteRegionManager() = default;

void OverwriteRegionManager::initialize(const ncp::Context& ctx, const BuildTarget& target)
{
    m_ctx = &ctx;
    m_target = &target;
}

void OverwriteRegionManager::setupOverwriteRegions()
{
    for (const auto& region : m_target->regions)
    {
        for (const auto& overwrite : region.overwrites)
        {
            std::string name = "overwrite_";
            name += Util::intToAddr(int(overwrite.startAddress), 8, false);
            if (region.destination != -1)
            {
                name += "_ov";
                name += std::to_string(region.destination);
            }

            auto overwriteRegion = std::make_unique<OverwriteRegionInfo>();
            overwriteRegion->startAddress = overwrite.startAddress;
            overwriteRegion->endAddress = overwrite.endAddress;
            overwriteRegion->destination = region.destination;
            overwriteRegion->assignedSections = {};
            overwriteRegion->usedSize = 0;
            overwriteRegion->name = name;
            m_overwriteRegions.push_back(std::move(overwriteRegion));

            if (m_ctx->isVerbose(ncp::VerboseTag::Section))
            {
                Log::out << OINFO << "Configured overwrite region: 0x" << std::hex << std::uppercase 
                    << overwrite.startAddress << "-0x" << overwrite.endAddress 
                    << " (size: " << std::dec << (overwrite.endAddress - overwrite.startAddress) 
                    << " bytes)" << std::endl;
            }
        }
    }
}

void OverwriteRegionManager::assignMeasuredSections(
    const std::vector<std::unique_ptr<SectionInfo>>& candidateSections,
    const std::vector<u32>& measuredSizes)
{
    if (m_overwriteRegions.empty())
        return;

    std::vector<PackRegion> regions;
    regions.reserve(m_overwriteRegions.size());
    for (auto& overwrite : m_overwriteRegions)
        regions.push_back({ overwrite->startAddress, overwrite->endAddress, overwrite->destination });

    std::vector<PackItem> items;
    items.reserve(candidateSections.size());
    for (std::size_t i = 0; i < candidateSections.size(); i++)
    {
        u32 size = i < measuredSizes.size() ? measuredSizes[i] : 0;
        items.push_back({
            size,
            candidateSections[i]->alignment,
            candidateSections[i]->unit->getTargetRegion()->destination,
            candidateSections[i]->isBss
        });
    }

    PackResult result = packOverwriteRegions(regions, items);

    std::vector<std::size_t> sectionCount(m_overwriteRegions.size(), 0);
    std::vector<u32> bssBytes(m_overwriteRegions.size(), 0);
    for (const PackPlacement& p : result.placements)
    {
        m_overwriteRegions[p.regionIndex]->assignedSections.push_back(candidateSections[p.itemIndex].get());
        sectionCount[p.regionIndex]++;
        if (items[p.itemIndex].lastResort)
            bssBytes[p.regionIndex] += items[p.itemIndex].size;
    }
    for (std::size_t r = 0; r < m_overwriteRegions.size(); r++)
        m_overwriteRegions[r]->usedSize = result.usedSize[r];

    m_stats = OverwriteStats{};
    for (std::size_t r = 0; r < m_overwriteRegions.size(); r++)
    {
        m_stats.usedBytes += result.usedSize[r];
        m_stats.capacityBytes += m_overwriteRegions[r]->endAddress - m_overwriteRegions[r]->startAddress;
    }
    m_stats.spilledBytes = result.spilledBytes;
    m_stats.spilledSections = result.spilled.size();

    // Utilization is informational, not a warning: a region that does not
    // fill up, or an item that spills to newcode, is a normal outcome, not a
    // modeling failure. Only a mismatch against what the linker actually
    // emits (checked in finalizeOverwritesWithElfData) is a real error. The
    // console gets a count from PatchMaker's "Patching" milestone instead.
    {
        Log::FileOnly fileOnly;
        for (std::size_t r = 0; r < m_overwriteRegions.size(); r++)
        {
            auto& overwrite = m_overwriteRegions[r];
            u32 capacity = overwrite->endAddress - overwrite->startAddress;
            Log::out << OINFO << "Overwrite region " << OSTR(overwrite->name) << ": "
                << result.usedSize[r] << "/" << capacity << " bytes used ("
                << sectionCount[r] << " section(s)";
            if (bssBytes[r] != 0)
                Log::out << ", " << bssBytes[r] << " of them bss";
            Log::out << ")" << std::endl;
        }

        if (!result.spilled.empty())
        {
            Log::out << OINFO << result.spilled.size() << " section(s) totaling " << result.spilledBytes
                << " bytes did not fit an overwrite region and will go to newcode instead." << std::endl;
        }
    }

    // Structure to store assignment information for table printing
    struct SectionAssignment {
        std::string sectionName;
        std::size_t sectionSize;
        u32 startAddress;
        u32 endAddress;
        core::CompilationUnit* unit;
        bool assigned;
    };
    std::vector<SectionAssignment> assignments;

    if (m_ctx->isVerbose(ncp::VerboseTag::Section))
    {
        std::unordered_map<std::size_t, std::size_t> regionForItem;
        for (const PackPlacement& p : result.placements)
            regionForItem[p.itemIndex] = p.regionIndex;
        std::unordered_set<std::size_t> spilledSet(result.spilled.begin(), result.spilled.end());

        for (std::size_t i = 0; i < candidateSections.size(); i++)
        {
            auto it = regionForItem.find(i);
            if (it != regionForItem.end())
            {
                auto& overwrite = m_overwriteRegions[it->second];
                assignments.push_back({
                    .sectionName = candidateSections[i]->name,
                    .sectionSize = items[i].size,
                    .startAddress = overwrite->startAddress,
                    .endAddress = overwrite->endAddress,
                    .unit = candidateSections[i]->unit,
                    .assigned = true
                });
            }
            else if (spilledSet.count(i))
            {
                assignments.push_back({
                    .sectionName = candidateSections[i]->name,
                    .sectionSize = items[i].size,
                    .startAddress = 0,
                    .endAddress = 0,
                    .unit = candidateSections[i]->unit,
                    .assigned = false
                });
            }
        }
    }

    // Print assignment table if verbose mode is enabled
    if (m_ctx->isVerbose(ncp::VerboseTag::Section) && !assignments.empty())
    {
        Log::out << ANSI_bCYAN "Assigned sections:" ANSI_RESET "\n" 
            << ANSI_bWHITE "SECTION_NAME" ANSI_RESET "                     " 
            << ANSI_bWHITE "SIZE" ANSI_RESET "     " 
            << ANSI_bWHITE "SOURCE" ANSI_RESET "        "
            << ANSI_bWHITE "OVERWRITE_REGION" ANSI_RESET "        " 
            << ANSI_bWHITE "STATUS" ANSI_RESET "        "  << std::endl;
        
        for (const auto& assignment : assignments)
        {
            // Section name - yellow for readability
            Log::out << ANSI_YELLOW << std::setw(64) << std::left << assignment.sectionName << ANSI_RESET << std::right << " ";
            
            // Size - white/cyan
            Log::out << ANSI_CYAN << std::setw(8) << std::dec << assignment.sectionSize << ANSI_RESET << "  ";

            Log::out << OSTR(assignment.unit->getObjectPath().string()) << ANSI_RESET << "  ";
            
            if (assignment.assigned)
            {
                // Address range - blue
                Log::out << ANSI_BLUE "0x" << std::setw(7) << std::hex << std::uppercase << assignment.startAddress 
                    << ANSI_BLUE "-0x" << std::setw(7) << assignment.endAddress << ANSI_RESET "  ";
                // Success status - green
                Log::out << ANSI_bGREEN << std::setw(8) << "ASSIGNED" << ANSI_RESET << std::endl;
            }
            else
            {
                // N/A - gray/white
                Log::out << ANSI_WHITE << std::setw(19) << "N/A" << ANSI_RESET << "  ";
                // Failed status - red
                Log::out << ANSI_bRED << std::setw(8) << "FAILED" << ANSI_RESET << std::endl;
            }
        }
    }
}

void OverwriteRegionManager::checkForConflictsWithPatches(const std::vector<std::unique_ptr<PatchInfo>>& patches)
{
    // Check that no patch is being written to an overwrite region
    bool foundPatchInOverwrite = false;
    for (const auto& patch : patches)
    {
        for (const auto& overwrite : m_overwriteRegions)
        {
            // Check if patch targets the same destination as the overwrite region
            if (patch->destAddressOv == overwrite->destination)
            {
                u32 patchEnd = patch->destAddress + patch->getOverwriteAmount();
                
                // Check if patch overlaps with overwrite region
                if (Util::overlaps(patch->destAddress, patchEnd, overwrite->startAddress, overwrite->endAddress))
                {
                    Log::out << OERROR
                        << "Patch " << OSTR(patch->getPrettyName()) << " (" << OSTR(patch->unit->getSourcePath().string()) 
                        << ") conflicts with overwrite region 0x" << std::hex << std::uppercase 
                        << overwrite->startAddress << "-0x" << overwrite->endAddress << std::endl;
                    foundPatchInOverwrite = true;
                }
            }
        }
    }
    if (foundPatchInOverwrite)
        throw ncp::exception("Patches targeting overwrite regions were detected.");
}

void OverwriteRegionManager::finalizeOverwritesWithElfData(const Elf32& elf)
{
    const Elf32_Ehdr& eh = elf.getHeader();
    auto sh_tbl = elf.getSectionHeaderTable();
    auto str_tbl = elf.getSection<char>(sh_tbl[eh.e_shstrndx]);

    // Gather overwrite section data
    for (const auto& overwrite : m_overwriteRegions)
    {
        overwrite->sectionIdx = -1;

		std::string overwriteSectionName = "." + overwrite->name;

        Elf32::forEachSection(eh, sh_tbl, str_tbl,
        [&](std::size_t sectionIdx, const Elf32_Shdr& section, std::string_view sectionName) -> bool {
            if (sectionName == overwriteSectionName)
            {
                overwrite->sectionIdx = sectionIdx;
                overwrite->sectionSize = section.sh_size;

                // The layout is measured with a real link, and the final
                // script places each section at that same measured alignment
                // in that same order, so the two must match exactly. A
                // mismatch here means the two links disagreed, which is a
                // real modeling bug, not a false alarm.
                if (overwrite->sectionSize != overwrite->usedSize)
                {
                    std::ostringstream oss;
                    oss << OERROR << "Overwrite region " << OSTR(overwrite->name)
                        << " at 0x" << std::hex << std::uppercase << overwrite->startAddress
                        << " has section size " << std::dec << section.sh_size
                        << " bytes, but expected " << overwrite->usedSize << " bytes." << std::endl;
                    throw ncp::exception(oss.str());
                }

                u32 maxSize = overwrite->endAddress - overwrite->startAddress;

                if (overwrite->sectionSize > maxSize)
                {
                    std::ostringstream oss;
                    oss << OERROR << "Overwrite region is smaller than the generated section "
                        << " (size: " << std::dec << overwrite->sectionSize << " bytes, max size: "  << maxSize << ")" << std::endl;
                    throw ncp::exception(oss.str());
                }
                
                if (m_ctx->isVerbose(ncp::VerboseTag::Patch))
                {
                    Log::out << OINFO << "Found overwrite region " << OSTR(overwrite->name) 
                        << " at 0x" << std::hex << std::uppercase << overwrite->startAddress
                        << " (size: " << std::dec << section.sh_size << " bytes)" << std::endl;
                }
                
                return true;
            }
            return false;
        });

        if (overwrite->sectionIdx == -1)
        {
            std::ostringstream oss;
            oss << "Failed to get section " << OSTR(overwriteSectionName) << " from ELF file.";
            throw ncp::exception(oss.str());
        }
    }
}

}
