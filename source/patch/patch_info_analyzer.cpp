#include "patch_info_analyzer.hpp"

#include <filesystem>
#include <iomanip>
#include <sstream>

#include "../main.hpp"
#include "../log.hpp"
#include "../except.hpp"
#include "../util.hpp"

namespace fs = std::filesystem;

struct PatchType {
    enum {
        Jump, Call, Hook, Over,
        SetJump, SetCall, SetHook,
        RtRepl,
        TJump, TCall, THook,
        SetTJump, SetTCall, SetTHook,
    };
};

static const char* s_patchTypeNames[] = {
    "jump", "call", "hook", "over",
    "setjump", "setcall", "sethook",
    "rtrepl",
    "tjump", "tcall", "thook",
    "settjump", "settcall", "setthook"
};

PatchInfoAnalyzer::PatchInfoAnalyzer() = default;
PatchInfoAnalyzer::~PatchInfoAnalyzer() = default;

void PatchInfoAnalyzer::initialize(
    const BuildTarget& target,
    const std::vector<std::unique_ptr<SourceFileJob>>& srcFileJobs,
    const std::filesystem::path& targetWorkDir
)
{
    m_target = &target;
    m_srcFileJobs = &srcFileJobs;
    m_targetWorkDir = &targetWorkDir;
}

void PatchInfoAnalyzer::forEachElfSection(
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

void PatchInfoAnalyzer::forEachElfSymbol(
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

void PatchInfoAnalyzer::gatherInfoFromObjects()
{
    fs::current_path(*m_targetWorkDir);

    Log::info("Getting patches from objects...");

    for (auto& srcFileJob : *m_srcFileJobs)
    {
        const fs::path& objPath = srcFileJob->objFilePath;

        if (Main::getVerbose())
            Log::out << ANSI_bYELLOW << objPath.string() << ANSI_RESET << std::endl;

        const BuildTarget::Region* region = srcFileJob->region;

        std::vector<GenericPatchInfo*> patchInfoForThisObj;

        if (!std::filesystem::exists(objPath))
            throw ncp::file_error(objPath, ncp::file_error::find);
        Elf32 elf;
        if (!elf.load(objPath))
            throw ncp::file_error(objPath, ncp::file_error::read);

        const Elf32_Ehdr& eh = elf.getHeader();
        auto sh_tbl = elf.getSectionHeaderTable();
        auto str_tbl = elf.getSection<char>(sh_tbl[eh.e_shstrndx]);
        const Elf32_Shdr* ncpSetSection = nullptr;
        const Elf32_Rel* ncpSetRel = nullptr;
        const Elf32_Sym* ncpSetRelSymTbl = nullptr;
        std::size_t ncpSetRelCount = 0;
        std::size_t ncpSetRelSymTblSize = 0;

        auto parseSymbol = [&](std::string_view symbolName, u32 symbolAddr, int sectionIdx, int sectionSize){
            std::string_view labelName = symbolName.substr(sectionIdx != -1 ? 5 : 4);

            std::size_t patchTypeNameEnd = labelName.find('_');
            if (patchTypeNameEnd == std::string::npos)
                return;

            std::string_view patchTypeName = labelName.substr(0, patchTypeNameEnd);
            std::size_t patchType = Util::indexOf(patchTypeName, s_patchTypeNames, sizeof(s_patchTypeNames) / sizeof(char*));
            if (patchType == -1)
            {
                Log::out << OWARN << "Found invalid patch type: " << patchTypeName << std::endl;
                return;
            }

            if (patchType == PatchType::Over && sectionIdx == -1)
            {
                Log::out << OWARN << "\"over\" patch must be a section type patch: " << patchTypeName << std::endl;
                return;
            }

            if (patchType == PatchType::RtRepl)
            {
                if (sectionIdx != -1) // we do not want the labels, those are placeholders
                {
                    m_rtreplPatches.emplace_back(new RtReplPatchInfo{
                        /*.symbol = */std::string(symbolName),
                        /*.job = */srcFileJob.get()
                    });
                }
                return;
            }

            bool forceThumb = false;
            if (patchType >= PatchType::TJump && patchType <= PatchType::THook)
            {
                patchType -= PatchType::TJump - PatchType::Jump;
                forceThumb = true;
            }
            else if (patchType >= PatchType::SetTJump && patchType <= PatchType::SetTHook)
            {
                patchType -= PatchType::SetTJump - PatchType::SetJump;
                forceThumb = true;
            }

            bool isNcpSet = false;
            if (patchType >= PatchType::SetJump && patchType <= PatchType::SetHook)
            {
                patchType -= PatchType::SetJump - PatchType::Jump;
                isNcpSet = true;
            }

            bool expectingOverlay = true;
            std::size_t addressNameStart = patchTypeNameEnd + 1;
            std::size_t addressNameEnd = labelName.find('_', addressNameStart);
            if (addressNameEnd == std::string::npos)
            {
                addressNameEnd = labelName.length();
                expectingOverlay = false;
            }
            std::string_view addressName = labelName.substr(addressNameStart, addressNameEnd - addressNameStart);
            u32 destAddress;
            try {
                destAddress = Util::addrToInt(std::string(addressName));
            } catch (std::exception& e) {
                Log::out << OWARN << "Found invalid address for patch: " << labelName << std::endl;
                return;
            }
            if (forceThumb)
                destAddress |= 1;

            int destAddressOv = -1;
            if (expectingOverlay)
            {
                std::size_t overlayNameStart = addressNameEnd + 1;
                std::size_t overlayNameEnd = labelName.length();
                std::string_view overlayName = labelName.substr(overlayNameStart, overlayNameEnd - overlayNameStart);
                if (!overlayName.starts_with("ov"))
                {
                    Log::out << OWARN << "Expected overlay definition in patch for: " << labelName << std::endl;
                    return;
                }
                try {
                    destAddressOv = Util::addrToInt(std::string(overlayName.substr(2)));
                } catch (std::exception& e) {
                    Log::out << OWARN << "Found invalid overlay for patch: " << labelName << std::endl;
                    return;
                }
            }

            for (auto& region : m_target->regions)
            {
                if (region.destination == destAddressOv && region.mode != BuildTarget::Mode::Append)
                {
                    std::ostringstream oss;
                    oss << OSTRa(symbolName) << " (" << OSTR(srcFileJob->srcFilePath.string())
                        << ") cannot be applied to an overlay that is not in " << OSTRa("append") << " mode.";
                    throw ncp::exception(oss.str());
                }
            }

            int srcAddressOv = patchType == PatchType::Over ? destAddressOv : region->destination;

            auto* patchInfoEntry = new GenericPatchInfo({
                .srcAddress = 0, // we do not yet know it, only after linkage
                .srcAddressOv = srcAddressOv,
                .destAddress = (destAddress & ~1),
                .destAddressOv = destAddressOv,
                .patchType = patchType,
                .sectionIdx = sectionIdx,
                .sectionSize = sectionSize,
                .isNcpSet = isNcpSet,
                .srcThumb = bool(symbolAddr & 1),
                .destThumb = bool(destAddress & 1),
                .symbol = std::string(symbolName),
                .job = srcFileJob.get()
            });

            patchInfoForThisObj.emplace_back(patchInfoEntry);
            m_patchInfo.emplace_back(patchInfoEntry);
        };

        // Find patches in sections
        forEachElfSection(eh, sh_tbl, str_tbl,
        [&](std::size_t sectionIdx, const Elf32_Shdr& section, std::string_view sectionName){
            if (sectionName.starts_with(".ncp_"))
            {
                if (ncpSetSection == nullptr && sectionName.substr(5).starts_with("set"))
                {
                    ncpSetSection = &section;
                    int dest = region->destination;
                    if (std::find(m_destWithNcpSet.begin(), m_destWithNcpSet.end(), dest) == m_destWithNcpSet.end())
                        m_destWithNcpSet.emplace_back(dest);
                    m_jobsWithNcpSet.emplace_back(srcFileJob.get());
                    return false;
                }
                parseSymbol(sectionName, 0, int(sectionIdx), int(section.sh_size));
            }
            else if (ncpSetRel == nullptr && sectionName == ".rel.ncp_set")
            {
                ncpSetRel = elf.getSection<Elf32_Rel>(section);
                ncpSetRelCount = section.sh_size / sizeof(Elf32_Rel);
                ncpSetRelSymTbl = elf.getSection<Elf32_Sym>(sh_tbl[section.sh_link]);
                ncpSetRelSymTblSize = sh_tbl[section.sh_link].sh_size / sizeof(Elf32_Sym);
            }
            return false;
        });

        // Find the functions corresponding to the patch to check if they are thumb
        forEachElfSymbol(elf, eh, sh_tbl,
        [&](const Elf32_Sym& symbol, std::string_view symbolName){
            if (ELF32_ST_TYPE(symbol.st_info) == STT_FUNC)
            {
                for (GenericPatchInfo* p : patchInfoForThisObj)
                {
                    // if function has the same section as the patch instruction section
                    if (p->sectionIdx == symbol.st_shndx)
                    {
                        p->srcThumb = symbol.st_value & 1;
                        break;
                    }
                }
            }
            return false;
        });

        // Find patches in symbols
        forEachElfSymbol(elf, eh, sh_tbl,
        [&](const Elf32_Sym& symbol, std::string_view symbolName){
            if (symbolName.starts_with("ncp_"))
            {
                std::string_view stemless = symbolName.substr(4);
                if (stemless != "dest")
                {
                    u32 addr = symbol.st_value;
                    if (stemless.starts_with("set")) // requires special care because of thumb function detection
                    {
                        if (ncpSetSection == nullptr)
                            throw ncp::exception("Found an ncp_set hook, but an \".ncp_set\" section does not exist!");

                        // Here we are just getting the address, so that we can know beforehand
                        // if the function is thumb or not. The final address is obtained after linkage.
                        auto& section = sh_tbl[symbol.st_shndx];
                        auto sectionData = elf.getSection<char>(section);
                        if (ncpSetRel == nullptr)
                        {
                            addr = Util::read<u32>(&sectionData[addr - section.sh_addr]);
                        }
                        else
                        {
                            for (int relIdx = 0; relIdx < ncpSetRelCount; relIdx++)
                            {
                                const Elf32_Rel& rel = ncpSetRel[relIdx];
                                if (rel.r_offset == addr) // found the corresponding relocation
                                {
                                    // layer after layer, we finally reach the symbol :p
                                    std::size_t symIdx = ELF32_R_SYM(rel.r_info);
                                    if (symIdx >= ncpSetRelSymTblSize)
                                    {
                                        std::ostringstream oss;
                                        oss << "Relocation entry with index " << relIdx
                                            << " in " << OSTR(".rel.ncp_set") << " section has an index of " << symIdx
                                            << " as linked symbol table entry but the symbol table only contains "
                                            << ncpSetRelSymTblSize << " entries.";
                                        throw ncp::exception(oss.str());
                                    }
                                    addr = ncpSetRelSymTbl[symIdx].st_value;
                                    break;
                                }
                            }
                        }
                    }
                    parseSymbol(symbolName, addr, -1, 0);
                }
            }
            return false;
        });

        // Find functions that should be external (label marked)
        for (GenericPatchInfo* p : patchInfoForThisObj)
        {
            if (p->sectionIdx == -1) // is patch instructed by label
                m_externSymbols.emplace_back(p->symbol);
        }

        // Find sections suitable to place in overwrites
        forEachElfSection(eh, sh_tbl, str_tbl,
        [&](std::size_t sectionIdx, const Elf32_Shdr& section, std::string_view sectionName){
            bool ncpSectionSupportsOverrideRegion =
                sectionName.starts_with(".ncp_jump") || 
                sectionName.starts_with(".ncp_call") || 
                sectionName.starts_with(".ncp_hook") ||
                sectionName.starts_with(".ncp_tjump") || 
                sectionName.starts_with(".ncp_tcall") || 
                sectionName.starts_with(".ncp_thook");
            
            if ((sectionName.starts_with(".ncp_") && !ncpSectionSupportsOverrideRegion) ||
                sectionName.starts_with(".rel") || 
                sectionName.starts_with(".debug") ||
                sectionName == ".shstrtab" || 
                sectionName == ".strtab" || 
                sectionName == ".symtab" ||
                section.sh_size == 0)
            {
                return false;
            }

            if (sectionName.starts_with(".text") || 
                sectionName.starts_with(".rodata") ||
                sectionName.starts_with(".data") ||
                ncpSectionSupportsOverrideRegion)
            {
                auto* sectionInfo = new SectionInfo{
                    .name = std::string(sectionName),
                    .size = section.sh_size,
                    .job = srcFileJob.get(),
                    .alignment = section.sh_addralign > 0 ? section.sh_addralign : 4
                };
                m_overwriteCandidateSections.emplace_back(sectionInfo);
            }
            return false;
        });

        if (Main::getVerbose())
        {
            if (patchInfoForThisObj.empty())
            {
                Log::out << "NO PATCHES" << std::endl;
            }
            else
            {
                Log::out << "SRC_ADDR_OV, DST_ADDR, DST_ADDR_OV, PATCH_TYPE, SEC_IDX, SEC_SIZE, NCP_SET, SRC_THUMB, DST_THUMB, SYMBOL" << std::endl;
                for (auto& p : patchInfoForThisObj)
                {
                    Log::out <<
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
        }
    }
    if (Main::getVerbose())
    {
        if (m_externSymbols.empty())
        {
            Log::out << "\nExternal symbols: NONE" << std::endl;
        }
        else
        {
            Log::out << "\nExternal symbols:\n";
            for (const std::string& sym : m_externSymbols)
                Log::out << sym << '\n';
            Log::out << std::flush;
        }
    }
}
