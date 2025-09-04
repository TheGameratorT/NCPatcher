#include "overwrite_region_manager.hpp"

#include <algorithm>
#include <iomanip>
#include <unordered_map>

#include "../app/application.hpp"
#include "../system/log.hpp"
#include "../system/except.hpp"
#include "../utils/util.hpp"

namespace ncp::patch {

OverwriteRegionManager::OverwriteRegionManager() = default;
OverwriteRegionManager::~OverwriteRegionManager() = default;

void OverwriteRegionManager::initialize(const BuildTarget& target, const DependencyResolver& dependencyResolver)
{
    m_target = &target;
	m_dependencyResolver = &dependencyResolver;
}

void OverwriteRegionManager::setupOverwriteRegions()
{
    Log::info("Setting up overwrite regions...");

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

            if (ncp::Application::isVerbose(ncp::VerboseTag::Section))
            {
                Log::out << OINFO << "Configured overwrite region: 0x" << std::hex << std::uppercase 
                    << overwrite.startAddress << "-0x" << overwrite.endAddress 
                    << " (size: " << std::dec << (overwrite.endAddress - overwrite.startAddress) 
                    << " bytes)" << std::endl;
            }
        }
    }
}

void OverwriteRegionManager::assignSectionsToOverwrites(std::vector<std::unique_ptr<SectionInfo>>& candidateSections)
{
    Log::info("Assigning sections to overwrite regions...");

    if (m_overwriteRegions.empty())
        return;

    // Group sections by destination
    std::unordered_map<int, std::vector<SectionInfo*>> sectionsByDest;
    for (auto& section : candidateSections)
    {
        int dest = section->unit->getTargetRegion()->destination;
        sectionsByDest[dest].emplace_back(section.get());
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

    // Process each destination
    for (auto& [dest, sections] : sectionsByDest)
    {
        // Get overwrite regions for this destination
        std::vector<OverwriteRegionInfo*> destOverwrites;
        for (auto& overwrite : m_overwriteRegions)
        {
            if (overwrite->destination == dest)
                destOverwrites.emplace_back(overwrite.get());
        }

        if (destOverwrites.empty())
            continue;

        // Sort sections by size (largest first for best fit)
        std::sort(sections.begin(), sections.end(), [](const SectionInfo* a, const SectionInfo* b){
            return a->size > b->size;
        });

        // Sort overwrite regions by available space (largest first)
        std::sort(destOverwrites.begin(), destOverwrites.end(), [](const OverwriteRegionInfo* a, const OverwriteRegionInfo* b){
            u32 sizeA = a->endAddress - a->startAddress - a->usedSize;
            u32 sizeB = b->endAddress - b->startAddress - b->usedSize;
            return sizeA > sizeB;
        });

        // Assign sections to overwrite regions using best fit algorithm
        for (auto* section : sections)
        {
            bool assigned = false;
            
            for (auto* overwrite : destOverwrites)
            {
                // Use the same forced alignment as in the linker script
                u32 forcedAlignment = 4;
                
                u32 currentPos = overwrite->startAddress + overwrite->usedSize;
                u32 alignedPos = (currentPos + forcedAlignment - 1) & ~(forcedAlignment - 1);
                u32 endPos = alignedPos + section->size;
                
                if (endPos <= overwrite->endAddress)
                {
                    // Assign this section to the overwrite region
                    overwrite->assignedSections.emplace_back(section);
                    overwrite->usedSize = endPos - overwrite->startAddress;
                    assigned = true;

                    // Store assignment info for table printing
                    if (ncp::Application::isVerbose(ncp::VerboseTag::Section))
                    {
                        assignments.push_back({
                            .sectionName = section->name,
                            .sectionSize = section->size,
                            .startAddress = overwrite->startAddress,
                            .endAddress = overwrite->endAddress,
                            .unit = section->unit,
                            .assigned = true
                        });
                    }
                    break;
                }
            }

            if (!assigned && ncp::Application::isVerbose(ncp::VerboseTag::Section))
            {
                assignments.push_back({
                    .sectionName = section->name,
                    .sectionSize = section->size,
                    .startAddress = 0,
                    .endAddress = 0,
                    .unit = section->unit,
                    .assigned = false
                });
            }
        }
    }

    // Apply final alignment to all overwrite regions (as the linker script does with ". = ALIGN(4);")
    for (auto& overwrite : m_overwriteRegions)
    {
        if (!overwrite->assignedSections.empty())
        {
            u32 forcedAlignment = 4;
            u32 alignedSize = (overwrite->usedSize + forcedAlignment - 1) & ~(forcedAlignment - 1);
            overwrite->usedSize = alignedSize;
        }
    }

    // Print assignment table if verbose mode is enabled
    if (ncp::Application::isVerbose(ncp::VerboseTag::Section) && !assignments.empty())
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

                if (overwrite->sectionSize != overwrite->usedSize)
                {
                    Log::out << OWARN << "Overwrite region " << OSTR(overwrite->name)
                        << " at 0x" << std::hex << std::uppercase << overwrite->startAddress
                        << " has section size " << std::dec << section.sh_size
                        << " bytes, but expected " << overwrite->usedSize << " bytes." << std::endl;
                }

                u32 maxSize = overwrite->endAddress - overwrite->startAddress;

                if (overwrite->sectionSize > maxSize)
                {
                    std::ostringstream oss;
                    oss << OERROR << "Overwrite region is smaller than the generated section "
                        << " (size: " << std::dec << overwrite->sectionSize << " bytes, max size: "  << maxSize << ")" << std::endl;
                    throw ncp::exception(oss.str());
                }
                
                if (ncp::Application::isVerbose(ncp::VerboseTag::Patch))
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
