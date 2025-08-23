#include "overwrite_region_manager.hpp"

#include <algorithm>
#include <iomanip>
#include <unordered_map>

#include "../main.hpp"
#include "../log.hpp"
#include "../util.hpp"

OverwriteRegionManager::OverwriteRegionManager() = default;
OverwriteRegionManager::~OverwriteRegionManager() = default;

void OverwriteRegionManager::initialize(const BuildTarget& target)
{
    m_target = &target;
}

void OverwriteRegionManager::setupOverwriteRegions()
{
    Log::info("Setting up overwrite regions...");

    for (const auto& region : m_target->regions)
    {
        for (const auto& overwrite : region.overwrites)
        {
            std::string memName = "overwrite_";
            memName += Util::intToAddr(int(overwrite.startAddress), 8, false);
            if (region.destination != -1)
            {
                memName += "_ov";
                memName += std::to_string(region.destination);
            }

            auto* overwriteRegion = new OverwriteRegionInfo{
                .startAddress = overwrite.startAddress,
                .endAddress = overwrite.endAddress,
                .destination = region.destination,
                .assignedSections = {},
                .usedSize = 0,
                .memName = memName
            };
            m_overwriteRegions.emplace_back(overwriteRegion);

            if (Main::getVerbose())
            {
                Log::out << OINFO << "Found overwrite region: 0x" << std::hex << std::uppercase 
                    << overwrite.startAddress << "-0x" << overwrite.endAddress 
                    << " (size: " << std::dec << (overwrite.endAddress - overwrite.startAddress) 
                    << " bytes)" << std::endl;
            }
        }
    }
}

void OverwriteRegionManager::assignSectionsToOverwrites(std::vector<std::unique_ptr<SectionInfo>>& candidateSections)
{
    if (m_overwriteRegions.empty())
        return;

    // Group sections by destination
    std::unordered_map<int, std::vector<SectionInfo*>> sectionsByDest;
    for (auto& section : candidateSections)
    {
        int dest = section->job->region->destination;
        sectionsByDest[dest].emplace_back(section.get());
    }

    // Structure to store assignment information for table printing
    struct SectionAssignment {
        std::string sectionName;
        std::size_t sectionSize;
        u32 startAddress;
        u32 endAddress;
		SourceFileJob* job;
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
                u32 currentPos = overwrite->startAddress + overwrite->usedSize;
                u32 alignedPos = (currentPos + section->alignment - 1) & ~(section->alignment - 1);
                u32 endPos = alignedPos + section->size;
                
                if (endPos <= overwrite->endAddress)
                {
                    // Assign this section to the overwrite region
                    overwrite->assignedSections.emplace_back(section);
                    overwrite->usedSize = endPos - overwrite->startAddress;
                    assigned = true;

                    // Store assignment info for table printing
                    if (Main::getVerbose())
                    {
                        assignments.push_back({
                            .sectionName = section->name,
                            .sectionSize = section->size,
                            .startAddress = overwrite->startAddress,
                            .endAddress = overwrite->endAddress,
							.job = section->job,
                            .assigned = true
                        });
                    }
                    break;
                }
            }

            if (!assigned && Main::getVerbose())
            {
                assignments.push_back({
                    .sectionName = section->name,
                    .sectionSize = section->size,
                    .startAddress = 0,
                    .endAddress = 0,
					.job = section->job,
                    .assigned = false
                });
            }
        }
    }

    // Print assignment table if verbose mode is enabled
    if (Main::getVerbose() && !assignments.empty())
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

            Log::out << OSTR(assignment.job->objFilePath.string()) << ANSI_RESET << "  ";
            
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
