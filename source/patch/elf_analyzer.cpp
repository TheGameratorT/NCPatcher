#include "elf_analyzer.hpp"

#include <filesystem>
#include <functional>
#include <iomanip>
#include <sstream>

#include "../main.hpp"
#include "../log.hpp"
#include "../except.hpp"
#include "../util.hpp"
#include "overwrite_region_manager.hpp"

static const char* s_patchTypeNames[] = {
    "jump", "call", "hook", "over",
    "setjump", "setcall", "sethook",
    "rtrepl",
    "tjump", "tcall", "thook",
    "settjump", "settcall", "setthook"
};

struct PatchType {
    enum {
        Jump, Call, Hook, Over,
        SetJump, SetCall, SetHook,
        RtRepl,
        TJump, TCall, THook,
        SetTJump, SetTCall, SetTHook,
    };
};

static u32 getPatchOverwriteAmount(const GenericPatchInfo* p)
{
    std::size_t pt = p->patchType;
    if (pt == PatchType::Over)
        return p->sectionSize;
    if (pt == PatchType::Jump && p->destThumb)
        return 8;
    return 4;
}

ElfAnalyzer::ElfAnalyzer() = default;
ElfAnalyzer::~ElfAnalyzer() = default;

void ElfAnalyzer::initialize(const std::filesystem::path& elfPath)
{
    m_elfPath = elfPath;
}

void ElfAnalyzer::forEachElfSection(
    const Elf32_Ehdr& eh, const Elf32_Shdr* sh_tbl, const char* str_tbl,
    const std::function<bool(std::size_t, const Elf32_Shdr&, std::string_view)>& cb
)
{
    for (std::size_t i = 0; i < eh.e_shnum; i++)
    {
        const Elf32_Shdr& sh = sh_tbl[i];
        std::string_view sectionName(&str_tbl[sh.sh_name]);
        if (cb(i, sh, sectionName))
            break;
    }
}

void ElfAnalyzer::forEachElfSymbol(
    const Elf32& elf, const Elf32_Ehdr& eh, const Elf32_Shdr* sh_tbl,
    const std::function<bool(const Elf32_Sym&, std::string_view)>& cb
)
{
    for (std::size_t i = 0; i < eh.e_shnum; i++)
    {
        const Elf32_Shdr& sh = sh_tbl[i];
        if ((sh.sh_type == SHT_SYMTAB) || (sh.sh_type == SHT_DYNSYM))
        {
            auto sym_tbl = elf.getSection<Elf32_Sym>(sh);
            auto sym_str_tbl = elf.getSection<char>(sh_tbl[sh.sh_link]);
            for (std::size_t j = 0; j < sh.sh_size / sizeof(Elf32_Sym); j++)
            {
                const Elf32_Sym& sym = sym_tbl[j];
                std::string_view symbolName(&sym_str_tbl[sym.st_name]);
                if (cb(sym, symbolName))
                    break;
            }
        }
    }
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
    forEachElfSymbol(*m_elf, eh, sh_tbl,
    [&](const Elf32_Sym& symbol, std::string_view symbolName){
        for (auto& p : patchInfo)
        {
            if (p->sectionIdx != -1) // patch is section
            {
                std::string_view nameAsLabel = std::string_view(p->symbol).substr(1);
                if (nameAsLabel == symbolName)
                {
                    p->srcAddress = symbol.st_value;
                    p->sectionIdx = symbol.st_shndx;
                    p->symbol = nameAsLabel;
                }
            }
            else
            {
                // This must run before fetching ncp_set section, otherwise ncp_set srcAddr will be overwritten
                if (p->symbol == symbolName)
                {
                    p->srcAddress = symbol.st_value;
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

    forEachElfSection(eh, sh_tbl, str_tbl,
    [&](std::size_t sectionIdx, const Elf32_Shdr& section, std::string_view sectionName){
        for (auto& p : patchInfo)
        {
            if (p->patchType == PatchType::Over)
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
            // found the ncp_set section, get all hook definitions stored there

            int srcAddrOv = -1;
            if (sectionName.length() != 8 && sectionName.substr(8).starts_with("_ov"))
            {
                try {
                    srcAddrOv = std::stoi(std::string(sectionName.substr(11)));
                } catch (std::exception& e) {
                    Log::out << OWARN << "Found invalid overlay reading ncp_set section: " << sectionName << std::endl;
                    return false;
                }
            }

            const char* sectionData = m_elf->getSection<char>(section);

            for (auto& p : patchInfo)
            {
                if (p->isNcpSet && p->srcAddressOv == srcAddrOv)
                {
                    u32 dataOffset = p->srcAddress - section.sh_addr;
                    if (dataOffset + 4 > section.sh_size)
                    {
                        std::ostringstream oss;
                        oss << "Tried to read " << OSTR(sectionName) << " data out of bounds.";
                        throw ncp::exception(oss.str());
                    }
                    // ncp_set comes with the THUMB bit, we must clear it!
                    p->srcAddress = Util::read<u32>(&sectionData[dataOffset]) & ~1;
                }
            }
        }
        return false;
    });

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
                    << OSTRa(a->symbol) << "[sz=" << aSz << "] (" << OSTR(a->job->srcFilePath.string()) << ") overlaps with "
                    << OSTRa(b->symbol) << "[sz=" << bSz << "] (" << OSTR(b->job->srcFilePath.string()) << ")\n";
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
                        << "Patch " << OSTR(patch->symbol) << " (" << OSTR(patch->job->srcFilePath.string()) 
                        << ") conflicts with overwrite region 0x" << std::hex << std::uppercase 
                        << overwrite->startAddress << "-0x" << overwrite->endAddress << std::endl;
                    foundPatchInOverwrite = true;
                }
            }
        }
    }
    if (foundPatchInOverwrite)
        throw ncp::exception("Patches targeting overwrite regions were detected.");
    
    if (Main::getVerbose())
    {
        Log::out << "Patches:\nSRC_ADDR, SRC_ADDR_OV, DST_ADDR, DST_ADDR_OV, PATCH_TYPE, SEC_IDX, SEC_SIZE, NCP_SET, SRC_THUMB, DST_THUMB, SYMBOL" << std::endl;
        for (auto& p : patchInfo)
        {
            Log::out <<
                std::setw(8) << std::hex << p->srcAddress << "  " <<
                std::setw(11) << std::dec << p->srcAddressOv << "  " <<
                std::setw(8) << std::hex << p->destAddress << "  " <<
                std::setw(11) << std::dec << p->destAddressOv << "  " <<
                std::setw(10) << s_patchTypeNames[p->patchType] << "  " <<
                std::setw(7) << std::dec << p->sectionIdx << "  " <<
                std::setw(8) << std::dec << p->sectionSize << "  " <<
                std::setw(7) << std::boolalpha << p->isNcpSet << "  " <<
                std::setw(9) << std::boolalpha << p->srcThumb << "  " <<
                std::setw(9) << std::boolalpha << p->destThumb << "  " <<
                std::setw(6) << p->symbol << std::endl;
        }
    }

    forEachElfSection(eh, sh_tbl, str_tbl,
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

    if (Main::getVerbose())
    {
        Log::out << "New Code Info:\nNAME    CODE_SIZE    BSS_SIZE" << std::endl;
        for (const auto& [dest, newcodeInfo] : m_newcodeDataForDest)
        {
            Log::out <<
                std::setw(8) << std::left << (dest == -1 ? "ARM" : ("OV" + std::to_string(dest))) << std::right <<
                std::setw(9) << std::dec << newcodeInfo->binSize << "    " <<
                std::setw(8) << std::dec << newcodeInfo->bssSize << std::endl;
        }
    }

    // Gather overwrite section data
    for (const auto& overwrite : overwriteRegions)
    {
        overwrite->sectionIdx = -1;

        forEachElfSection(eh, sh_tbl, str_tbl,
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
                
                if (Main::getVerbose())
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
