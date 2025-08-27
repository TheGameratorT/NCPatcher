#include "elf_analyzer.hpp"

#include <filesystem>
#include <functional>
#include <iomanip>
#include <sstream>
#include <unordered_set>
#include <algorithm>

#include "../app/application.hpp"
#include "../system/log.hpp"
#include "../system/except.hpp"
#include "../utils/util.hpp"
#include "overwrite_region_manager.hpp"

static const char* s_patchTypeNames[] = {
    "jump", "call", "hook", "over",
    "setjump", "setcall", "sethook",
    "rtrepl",
    "tjump", "tcall", "thook",
    "settjump", "settcall", "setthook"
};

static u32 getPatchOverwriteAmount(const GenericPatchInfo* p)
{
    std::size_t pt = p->patchType;
    if (pt == patch::PatchType::Over)
        return p->sectionSize;
    if (pt == patch::PatchType::Jump && p->destThumb)
        return 8;
    return 4;
}

ElfAnalyzer::ElfAnalyzer() = default;
ElfAnalyzer::~ElfAnalyzer() = default;

void ElfAnalyzer::initialize(const std::filesystem::path& elfPath)
{
    m_elfPath = elfPath;
}

void ElfAnalyzer::loadElfFile()
{
    if (!std::filesystem::exists(m_elfPath))
        throw ncp::file_error(m_elfPath, ncp::file_error::find);

    m_elf = std::make_unique<Elf32>();
    if (!m_elf->load(m_elfPath))
        throw ncp::file_error(m_elfPath, ncp::file_error::read);
}

void ElfAnalyzer::unloadElfFile()
{
    m_elf = nullptr;
}

void ElfAnalyzer::gatherInfoFromElf(
    std::vector<std::unique_ptr<GenericPatchInfo>>& patchInfo,
    const std::vector<std::unique_ptr<OverwriteRegionInfo>>& overwriteRegions
)
{
    Log::info("Getting patches from elf...");

    const Elf32_Ehdr& eh = m_elf->getHeader();
    auto sh_tbl = m_elf->getSectionHeaderTable();
    auto str_tbl = m_elf->getSection<char>(sh_tbl[eh.e_shstrndx]);

    // Update the patch info with new values
    Elf32::forEachSymbol(*m_elf, eh, sh_tbl,
    [&](const Elf32_Sym& symbol, std::string_view symbolName){
        for (auto& p : patchInfo)
        {
            if (p->sourceType == PatchSourceType::Section)
            {
				if (p->isNcpSet)
				{
					if (p->symbol == symbolName)
					{
						p->sectionIdx = symbol.st_shndx;
					}
				}
				else
				{
					std::string_view nameAsLabel = std::string_view(p->symbol).substr(1);
					if (nameAsLabel == symbolName)
					{
						p->srcAddress = symbol.st_value;
						p->sectionIdx = symbol.st_shndx;
						p->symbol = nameAsLabel;
					}
				}
            }
            else
            {
                // This must run before fetching ncp_set section, otherwise ncp_set srcAddr will be overwritten
                if (p->symbol == symbolName)
                {
                    p->srcAddress = symbol.st_value & ~1;
                    p->sectionIdx = symbol.st_shndx;
                }
            }
        }
        if (symbolName.starts_with("ncp_autogendata"))
        {
            int srcAddrOv = -1;
            if (symbolName.length() != 15 && symbolName.substr(15).starts_with("_ov"))
            {
                try {
                    srcAddrOv = std::stoi(std::string(symbolName.substr(18)));
                } catch (std::exception& e) {
                    Log::out << OWARN << "Found invalid overlay parsing ncp_autogendata symbol: " << symbolName << std::endl;
                    return false;
                }
            }
            auto* info = new AutogenDataInfo();
            info->address = symbol.st_value;
            info->curAddress = symbol.st_value;
            m_autogenDataInfoForDest.emplace(srcAddrOv, info);
        }
        return false;
    });

    Elf32::forEachSection(eh, sh_tbl, str_tbl,
    [&](std::size_t sectionIdx, const Elf32_Shdr& section, std::string_view sectionName){
        for (auto& p : patchInfo)
        {
            if (p->patchType == patch::PatchType::Over)
            {
                if (p->symbol == sectionName)
                {
                    p->srcAddress = section.sh_addr; // should be the same as the destination
                    p->sectionIdx = int(sectionIdx);
                }
            }
        }
        if (sectionName.starts_with(".ncp_set"))
        {
            // Handle ncp_set sections directly - each section corresponds to a specific patch
            const char* sectionData = m_elf->getSection<char>(section);
            
            // Find the patch that corresponds to this section
            for (auto& p : patchInfo)
            {
                if (p->isNcpSet && p->symbol == sectionName)
                {
                    if (section.sh_size != 4)
                    {
                        std::ostringstream oss;
                        oss << "ncp_set section " << OSTR(sectionName) << " should be exactly 4 bytes, but is " << section.sh_size << " bytes.";
                        throw ncp::exception(oss.str());
                    }
                    
                    // Read the function pointer from the section data
                    // ncp_set comes with the THUMB bit, we must clear it!
                    p->srcAddress = Util::read<u32>(&sectionData[0]) & ~1;
                    // Also determine if the source function is thumb from the LSB
                    p->srcThumb = bool(Util::read<u32>(&sectionData[0]) & 1);
                }
            }
        }
        return false;
    });

	// TODO: the overlap checks should probably be moved to before linking, keep here for now

    // Check if any overlapping patches exist
    bool foundOverlapping = false;
    for (std::size_t i = 0; i < patchInfo.size(); i++)
    {
        auto& a = patchInfo[i];
        for (std::size_t j = i + 1; j < patchInfo.size(); j++)
        {
            auto& b = patchInfo[j];
            if (a->destAddressOv != b->destAddressOv)
                continue;
            u32 aSz = getPatchOverwriteAmount(a.get());
            u32 bSz = getPatchOverwriteAmount(b.get());
            if (Util::overlaps(a->destAddress, a->destAddress + aSz, b->destAddress, b->destAddress + bSz))
            {
                Log::out << OERROR
                    << OSTRa(a->symbol) << "[sz=" << aSz << "] (" << OSTR(a->unit->getSourcePath().string()) << ") overlaps with "
                    << OSTRa(b->symbol) << "[sz=" << bSz << "] (" << OSTR(b->unit->getSourcePath().string()) << ")\n";
                foundOverlapping = true;
            }
        }
    }
    if (foundOverlapping)
        throw ncp::exception("Overlapping patches were detected.");
    
    // Check that no patch is being written to an overwrite region
    bool foundPatchInOverwrite = false;
    for (const auto& patch : patchInfo)
    {
        for (const auto& overwrite : overwriteRegions)
        {
            // Check if patch targets the same destination as the overwrite region
            if (patch->destAddressOv == overwrite->destination)
            {
                u32 patchEnd = patch->destAddress + getPatchOverwriteAmount(patch.get());
                
                // Check if patch overlaps with overwrite region
                if (Util::overlaps(patch->destAddress, patchEnd, overwrite->startAddress, overwrite->endAddress))
                {
                    Log::out << OERROR
                        << "Patch " << OSTR(patch->formatPatchDescriptor()) << " (" << OSTR(patch->unit->getSourcePath().string()) 
                        << ") conflicts with overwrite region 0x" << std::hex << std::uppercase 
                        << overwrite->startAddress << "-0x" << overwrite->endAddress << std::endl;
                    foundPatchInOverwrite = true;
                }
            }
        }
    }
    if (foundPatchInOverwrite)
        throw ncp::exception("Patches targeting overwrite regions were detected.");
    
    if (ncp::Application::isVerbose(ncp::VerboseTag::Patch))
    {
        Log::out << ANSI_bCYAN "Patches (post-ELF analysis):" ANSI_RESET "\n"
        	<< ANSI_bYELLOW "Note: Fields marked with * are populated/updated during ELF analysis phase" ANSI_RESET << std::endl;

        Log::out << ANSI_bWHITE "  SRC_ADDR" ANSI_RESET "  "
            << ANSI_bWHITE "SRC_ADDR_OV" ANSI_RESET "    "
            << ANSI_bWHITE "DST_ADDR" ANSI_RESET "  "
            << ANSI_bWHITE "DST_ADDR_OV" ANSI_RESET "  "
            << ANSI_bWHITE "PATCH_TYPE" ANSI_RESET "   "
            << ANSI_bWHITE "SEC_IDX" ANSI_RESET "  "
            << ANSI_bWHITE "SEC_SIZE" ANSI_RESET "  "
            << ANSI_bWHITE "NCP_SET" ANSI_RESET "  "
            << ANSI_bWHITE "SRC_THUMB" ANSI_RESET "  "
            << ANSI_bWHITE "DST_THUMB" ANSI_RESET "  "
            << ANSI_bWHITE "SOURCE_TYPE" ANSI_RESET "  "
            << ANSI_bWHITE "SYMBOL" ANSI_RESET << std::endl;
        for (auto& p : patchInfo)
        {
			if (ncp::Application::isVerbose(ncp::VerboseTag::NoLib) && p->unit->getType() == core::CompilationUnitType::LibraryFile)
				continue;

            Log::out <<
                ANSI_CYAN << std::setw(10) << Util::intToAddr(p->srcAddress, 8) << "*";
            Log::out << ANSI_RESET " " <<
                ANSI_YELLOW << std::setw(11) << std::dec << p->srcAddressOv << ANSI_RESET "  " <<
                ANSI_BLUE << std::setw(8) << Util::intToAddr(p->destAddress, 8) << ANSI_RESET "  " <<
                ANSI_YELLOW << std::setw(11) << std::dec << p->destAddressOv << ANSI_RESET "  " <<
                ANSI_MAGENTA << std::setw(10) << s_patchTypeNames[p->patchType] << ANSI_RESET "  " <<
                ANSI_WHITE << std::setw(8) << std::dec << p->sectionIdx << "*";
            Log::out << ANSI_RESET " " <<
                ANSI_WHITE << std::setw(8) << std::dec << p->sectionSize << ANSI_RESET "  " <<
                ANSI_GREEN << std::setw(7) << std::boolalpha << p->isNcpSet << ANSI_RESET "  " <<
                ANSI_GREEN << std::setw(9) << std::boolalpha << p->srcThumb;
            // Mark srcThumb field populated during ELF analysis for ncp_set patches
            Log::out << (p->isNcpSet ? "*" : " ");
            Log::out << ANSI_RESET " " <<
                ANSI_GREEN << std::setw(9) << std::boolalpha << p->destThumb << ANSI_RESET "  " <<
                ANSI_bYELLOW << std::setw(11) << toString(p->sourceType) << ANSI_RESET "  " <<
                ANSI_WHITE << p->symbol;
            Log::out << ANSI_RESET << std::endl;
        }
    }

    Elf32::forEachSection(eh, sh_tbl, str_tbl,
    [&](std::size_t sectionIdx, const Elf32_Shdr& section, std::string_view sectionName){
        auto insertSection = [&](int dest, bool isBss){
            auto& newcodeInfo = m_newcodeDataForDest[dest];
            if (newcodeInfo == nullptr)
                newcodeInfo = std::make_unique<NewcodePatch>();

            (isBss ? newcodeInfo->bssData : newcodeInfo->binData) = m_elf->getSection<u8>(section);
            (isBss ? newcodeInfo->bssSize : newcodeInfo->binSize) = section.sh_size;
            (isBss ? newcodeInfo->bssAlign : newcodeInfo->binAlign) = section.sh_addralign;
        };

        if (sectionName.starts_with(".arm"))
        {
            insertSection(-1, sectionName.substr(5) == "bss");
        }
        else if (sectionName.starts_with(".ov") && !sectionName.starts_with(".overwrite"))
        {
            std::size_t pos = sectionName.find('.', 3);
            if (pos != std::string::npos)
            {
                int dest = std::stoi(std::string(sectionName.substr(3, pos - 3)));
                insertSection(dest, sectionName.substr(pos + 1) == "bss");
            }
        }
        return false;
    });

    if (ncp::Application::isVerbose(ncp::VerboseTag::Elf))
    {
        Log::out << ANSI_bCYAN "New Code Info:" ANSI_RESET "\n"
            << ANSI_bWHITE "NAME" ANSI_RESET "    "
            << ANSI_bWHITE "CODE_SIZE" ANSI_RESET "    "
            << ANSI_bWHITE "BSS_SIZE" ANSI_RESET << std::endl;
        for (const auto& [dest, newcodeInfo] : m_newcodeDataForDest)
        {
            Log::out <<
                ANSI_YELLOW << std::setw(8) << std::left << (dest == -1 ? "ARM" : ("OV" + std::to_string(dest))) << ANSI_RESET << std::right <<
                ANSI_CYAN << std::setw(9) << std::dec << newcodeInfo->binSize << ANSI_RESET "    " <<
                ANSI_CYAN << std::setw(8) << std::dec << newcodeInfo->bssSize << ANSI_RESET << std::endl;
        }
    }

    // Gather overwrite section data
    for (const auto& overwrite : overwriteRegions)
    {
        overwrite->sectionIdx = -1;

        Elf32::forEachSection(eh, sh_tbl, str_tbl,
        [&](std::size_t sectionIdx, const Elf32_Shdr& section, std::string_view sectionName) -> bool {
            if (sectionName == "." + overwrite->memName)
            {
                overwrite->sectionIdx = sectionIdx;
                overwrite->sectionSize = section.sh_size;

                if (overwrite->sectionSize != overwrite->usedSize)
                {
                    Log::out << OWARN << "Overwrite region " << OSTR(overwrite->memName)
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
                    Log::out << OINFO << "Found overwrite region " << OSTR(overwrite->memName) 
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
            oss << "Failed to get section " << OSTR(overwrite->memName) << " from ELF file.";
            throw ncp::exception(oss.str());
        }
    }
}
