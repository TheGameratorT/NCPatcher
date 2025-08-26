#include "patch_info_analyzer.hpp"

#include <filesystem>
#include <iomanip>
#include <sstream>

#include "../app/application.hpp"
#include "../system/log.hpp"
#include "../system/except.hpp"
#include "../utils/util.hpp"
#include "../formats/archive.hpp"

namespace fs = std::filesystem;
using namespace patch;

PatchInfoAnalyzer::PatchInfoAnalyzer() = default;
PatchInfoAnalyzer::~PatchInfoAnalyzer() = default;

void PatchInfoAnalyzer::initialize(
    const BuildTarget& target,
    const std::filesystem::path& targetWorkDir,
    core::CompilationUnitManager& compilationUnitMgr
)
{
    m_target = &target;
    m_targetWorkDir = &targetWorkDir;
	m_compilationUnitMgr = &compilationUnitMgr;
}

void PatchInfoAnalyzer::gatherInfoFromObjects()
{
    fs::current_path(*m_targetWorkDir);
    Log::info("Getting patches from objects...");

    for (auto& unit : m_compilationUnitMgr->getUnits())
    {
        processObjectFile(unit.get());
    }

    printExternSymbols();
}

void PatchInfoAnalyzer::processObjectFile(core::CompilationUnit* unit)
{
    const fs::path& objPath = unit->getObjectPath();

    if (ncp::Application::isVerbose())
        Log::out << ANSI_bYELLOW << objPath.string() << ANSI_RESET << std::endl;

    // Try to get ELF directly from the compilation unit first
    Elf32* elf = unit->getElf();

    const Elf32_Ehdr& eh = elf->getHeader();
    auto sh_tbl = elf->getSectionHeaderTable();
    auto str_tbl = elf->getSection<char>(sh_tbl[eh.e_shstrndx]);
    
    std::vector<GenericPatchInfo*> patchInfoForThisObj;
    const Elf32_Shdr* ncpSetSection = nullptr;
    const Elf32_Rel* ncpSetRel = nullptr;
    const Elf32_Sym* ncpSetRelSymTbl = nullptr;
    std::size_t ncpSetRelCount = 0;
    std::size_t ncpSetRelSymTblSize = 0;

    // Process sections
    processElfSections(*elf, eh, sh_tbl, str_tbl, unit, patchInfoForThisObj,
                      ncpSetSection, ncpSetRel, ncpSetRelSymTbl, 
                      ncpSetRelCount, ncpSetRelSymTblSize);

    // Update thumb information for patches
    updatePatchThumbInfo(*elf, eh, sh_tbl, patchInfoForThisObj);

    // Process symbols
    processElfSymbols(*elf, eh, sh_tbl, unit, patchInfoForThisObj,
                     ncpSetSection, ncpSetRel, ncpSetRelSymTbl,
                     ncpSetRelCount, ncpSetRelSymTblSize);

    // Resolve symver patches to their real symbol names
    resolveSymverPatches(*elf, eh, sh_tbl, patchInfoForThisObj);

    // Find functions that should be external (label and symver marked)
    for (GenericPatchInfo* p : patchInfoForThisObj)
    {
        if (p->sourceType == PatchSourceType::Label)
		{
            m_externSymbols.emplace_back(p->symbol);
		}
		else if (p->sourceType == PatchSourceType::Symver)
		{
			// Symver is stackable and uses the actual target
			// function symbol so we must filter out duplicates.
			if (std::find(m_externSymbols.begin(), m_externSymbols.end(), p->symbol) == m_externSymbols.end())
			{
            	m_externSymbols.emplace_back(p->symbol);
			}
		}
    }

    // Collect sections suitable for overwrites
    collectOverwriteCandidateSections(*elf, eh, sh_tbl, str_tbl, unit);

    printPatchInfoForObject(patchInfoForThisObj);
}

PatchInfoAnalyzer::ParsedPatchInfo PatchInfoAnalyzer::parsePatchTypeAndAddress(std::string_view labelName)
{
    ParsedPatchInfo info;
    
    std::size_t patchTypeNameEnd = labelName.find('_');
    if (patchTypeNameEnd == std::string::npos)
        return info;

    std::string_view patchTypeName = labelName.substr(0, patchTypeNameEnd);
    std::size_t patchType = Util::indexOf(patchTypeName, patch::PatchTypeUtils::patchTypeNames, patch::PatchTypeUtils::numPatchTypes);
    if (patchType == -1)
    {
        Log::out << OWARN << "Found invalid patch type: " << patchTypeName << std::endl;
        return info;
    }

    info.patchType = patchType;

    bool expectingOverlay = true;
    std::size_t addressNameStart = patchTypeNameEnd + 1;
    std::size_t addressNameEnd = labelName.find('_', addressNameStart);
    if (addressNameEnd == std::string::npos)
    {
        addressNameEnd = labelName.length();
        expectingOverlay = false;
    }

    std::string_view addressName = labelName.substr(addressNameStart, addressNameEnd - addressNameStart);
    try {
        info.destAddress = parseAddress(addressName);
    } catch (std::exception& e) {
        Log::out << OWARN << "Found invalid address for patch: " << labelName << std::endl;
        return info;
    }

    info.destAddressOv = -1;
    if (expectingOverlay)
    {
        std::size_t overlayNameStart = addressNameEnd + 1;
        std::size_t overlayNameEnd = labelName.length();
        std::string_view overlayName = labelName.substr(overlayNameStart, overlayNameEnd - overlayNameStart);
        if (!overlayName.starts_with("ov"))
        {
            Log::out << OWARN << "Expected overlay definition in patch for: " << labelName << std::endl;
            return info;
        }
        try {
            info.destAddressOv = parseOverlay(overlayName.substr(2));
        } catch (std::exception& e) {
            Log::out << OWARN << "Found invalid overlay for patch: " << labelName << std::endl;
            return info;
        }
    }

    normalizePatchType(info);
    info.isValid = true;
    return info;
}

PatchInfoAnalyzer::ParsedPatchInfo PatchInfoAnalyzer::parseSymverPatchTypeAndAddress(std::string_view labelName)
{
    ParsedPatchInfo info;
    
    std::size_t patchTypeNameEnd = labelName.find('_');
    if (patchTypeNameEnd == std::string::npos)
        return info;

    std::string_view patchTypeName = labelName.substr(0, patchTypeNameEnd);
    std::size_t patchType = Util::indexOf(patchTypeName, patch::PatchTypeUtils::patchTypeNames, patch::PatchTypeUtils::numPatchTypes);
    if (patchType == -1)
    {
        Log::out << OWARN << "Found invalid patch type: " << patchTypeName << std::endl;
        return info;
    }

    info.patchType = patchType;

    // For symver patches, find the address (second component)
    std::size_t addressNameStart = patchTypeNameEnd + 1;
    std::size_t addressNameEnd = labelName.find('_', addressNameStart);
    if (addressNameEnd == std::string::npos)
    {
        addressNameEnd = labelName.length();
    }

    std::string_view addressName = labelName.substr(addressNameStart, addressNameEnd - addressNameStart);
    try {
        info.destAddress = parseAddress(addressName);
    } catch (std::exception& e) {
        Log::out << OWARN << "Found invalid address for patch: " << labelName << std::endl;
        return info;
    }

    info.destAddressOv = -1;
    
    // For symver patches, check if there's an overlay specification after the address
    // Format: <type>_<address>[_ov<overlay>]_<unique ids...>
    if (addressNameEnd < labelName.length())
    {
        std::size_t overlayNameStart = addressNameEnd + 1;
        std::size_t overlayNameEnd = labelName.find('_', overlayNameStart);
        if (overlayNameEnd == std::string::npos)
        {
            overlayNameEnd = labelName.length();
        }
        
        std::string_view potentialOverlay = labelName.substr(overlayNameStart, overlayNameEnd - overlayNameStart);
        if (potentialOverlay.starts_with("ov"))
        {
            try {
                info.destAddressOv = parseOverlay(potentialOverlay.substr(2));
            } catch (std::exception& e) {
                Log::out << OWARN << "Found invalid overlay for symver patch: " << labelName << std::endl;
                return info;
            }
        }
    }

    normalizePatchType(info);
    info.isValid = true;
    return info;
}

u32 PatchInfoAnalyzer::parseAddress(std::string_view addressStr) const
{
    return Util::addrToInt(std::string(addressStr));
}

int PatchInfoAnalyzer::parseOverlay(std::string_view overlayStr) const
{
    return Util::addrToInt(std::string(overlayStr));
}

void PatchInfoAnalyzer::normalizePatchType(ParsedPatchInfo& info) const
{
    info.forceThumb = false;
    info.isNcpSet = false;

    if (info.patchType >= patch::PatchType::TJump && info.patchType <= patch::PatchType::THook)
    {
        info.patchType -= patch::PatchType::TJump - patch::PatchType::Jump;
        info.forceThumb = true;
    }
    else if (info.patchType >= patch::PatchType::SetTJump && info.patchType <= patch::PatchType::SetTHook)
    {
        info.patchType -= patch::PatchType::SetTJump - patch::PatchType::SetJump;
        info.forceThumb = true;
    }

    if (info.patchType >= patch::PatchType::SetJump && info.patchType <= patch::PatchType::SetHook)
    {
        info.patchType -= patch::PatchType::SetJump - patch::PatchType::Jump;
        info.isNcpSet = true;
    }

    if (info.forceThumb)
        info.destAddress |= 1;
}

std::unique_ptr<GenericPatchInfo> PatchInfoAnalyzer::createPatchInfo(
    const ParsedPatchInfo& parsedInfo,
    std::string_view symbolName,
    u32 symbolAddr,
    int sectionIdx,
    int sectionSize,
    core::CompilationUnit* unit,
    patch::PatchSourceType sourceType
) const
{
    const BuildTarget::Region* region = unit->getTargetRegion();
    int srcAddressOv = (parsedInfo.patchType == patch::PatchType::Over) ? parsedInfo.destAddressOv : region->destination;

    auto patchInfoEntry = std::make_unique<GenericPatchInfo>();
    patchInfoEntry->srcAddress = symbolAddr; // we do not yet know it, only after linkage, but we need it to resolve symver
    patchInfoEntry->srcAddressOv = srcAddressOv;
    patchInfoEntry->destAddress = (parsedInfo.destAddress & ~1);
    patchInfoEntry->destAddressOv = parsedInfo.destAddressOv;
    patchInfoEntry->patchType = parsedInfo.patchType;
    patchInfoEntry->sectionIdx = sectionIdx;
    patchInfoEntry->sectionSize = sectionSize;
    patchInfoEntry->isNcpSet = parsedInfo.isNcpSet;
    patchInfoEntry->srcThumb = bool(symbolAddr & 1);
    patchInfoEntry->destThumb = bool(parsedInfo.destAddress & 1);
    patchInfoEntry->symbol = std::string(symbolName);
    patchInfoEntry->unit = unit;
    patchInfoEntry->sourceType = sourceType;

    return patchInfoEntry;
}

void PatchInfoAnalyzer::validatePatchForRegion(const ParsedPatchInfo& parsedInfo, std::string_view symbolName, core::CompilationUnit* unit) const
{
    const auto* targetRegion = m_target->getRegionByDestination(parsedInfo.destAddressOv);
    if (targetRegion && targetRegion->mode != BuildTarget::Mode::Append)
    {
        std::ostringstream oss;
        oss << OSTRa(symbolName) << " (" << OSTR(unit->getSourcePath().string())
            << ") cannot be applied to an overlay that is not in " << OSTRa("append") << " mode.";
        throw ncp::exception(oss.str());
    }
}

bool PatchInfoAnalyzer::isValidSectionForOverwrites(std::string_view sectionName, const Elf32_Shdr& section) const
{
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

    return sectionName.starts_with(".text") || 
           sectionName.starts_with(".rodata") ||
           sectionName.starts_with(".init_array") ||
           sectionName.starts_with(".data") ||
           sectionName.starts_with(".bss") ||
           ncpSectionSupportsOverrideRegion;
}

void PatchInfoAnalyzer::processElfSections(const Elf32& elf, const Elf32_Ehdr& eh, const Elf32_Shdr* sh_tbl, 
                                         const char* str_tbl, core::CompilationUnit* unit,
                                         std::vector<GenericPatchInfo*>& patchInfoForThisObj,
                                         const Elf32_Shdr*& ncpSetSection, const Elf32_Rel*& ncpSetRel,
                                         const Elf32_Sym*& ncpSetRelSymTbl, std::size_t& ncpSetRelCount, 
                                         std::size_t& ncpSetRelSymTblSize)
{
    Elf32::forEachSection(eh, sh_tbl, str_tbl,
    [&](std::size_t sectionIdx, const Elf32_Shdr& section, std::string_view sectionName){
        if (sectionName.starts_with(".ncp_"))
        {
            if (ncpSetSection == nullptr && sectionName.substr(5).starts_with("set"))
            {
                ncpSetSection = &section;
                handleNcpSetSection(section, unit);
                return false;
            }
            parseSectionSymbol(sectionName, int(sectionIdx), int(section.sh_size), unit, patchInfoForThisObj);
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
}

void PatchInfoAnalyzer::processElfSymbols(const Elf32& elf, const Elf32_Ehdr& eh, const Elf32_Shdr* sh_tbl,
                                        core::CompilationUnit* unit, std::vector<GenericPatchInfo*>& patchInfoForThisObj,
                                        const Elf32_Shdr* ncpSetSection, const Elf32_Rel* ncpSetRel,
                                        const Elf32_Sym* ncpSetRelSymTbl, std::size_t ncpSetRelCount,
                                        std::size_t ncpSetRelSymTblSize)
{
    Elf32::forEachSymbol(elf, eh, sh_tbl,
    [&](const Elf32_Sym& symbol, std::string_view symbolName){
        if (symbolName.starts_with("ncp_"))
        {
            parseRegularSymbol(symbolName, symbol.st_shndx, symbol.st_value, unit, patchInfoForThisObj,
                             ncpSetSection, ncpSetRel, ncpSetRelSymTbl, 
                             ncpSetRelCount, ncpSetRelSymTblSize, elf, sh_tbl);
        }
        else if (symbolName.starts_with("__ncp_"))
        {
            // Handle symver patches: __ncp_<type>_<address>[_ov<overlay>]_<unique ids to ignore>
            parseSymverSymbol(symbolName, symbol.st_shndx, symbol.st_value, unit, patchInfoForThisObj);
        }
        return false;
    });
}

void PatchInfoAnalyzer::updatePatchThumbInfo(const Elf32& elf, const Elf32_Ehdr& eh, const Elf32_Shdr* sh_tbl,
                                            std::vector<GenericPatchInfo*>& patchInfoForThisObj)
{
    // Find the functions corresponding to the patch to check if they are thumb
    Elf32::forEachSymbol(elf, eh, sh_tbl,
    [&](const Elf32_Sym& symbol, std::string_view symbolName){
        if (ELF32_ST_TYPE(symbol.st_info) == STT_FUNC)
        {
			// no need to check if it is a section patch because we haven't parsed symbol patches yet
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
}

void PatchInfoAnalyzer::resolveSymverPatches(const Elf32& elf, const Elf32_Ehdr& eh, const Elf32_Shdr* sh_tbl,
                                            std::vector<GenericPatchInfo*>& patchInfoForThisObj)
{
    // For each symver patch, find the real symbol that has the same address and doesn't contain '@'
    for (GenericPatchInfo* p : patchInfoForThisObj)
    {
        if (p->sourceType != PatchSourceType::Symver)
            continue;
            
        u32 symverAddr = p->srcAddress;
        std::string realSymbolName;
        
        // Find the first symbol with the same address that doesn't contain '@'
        Elf32::forEachSymbol(elf, eh, sh_tbl,
        [&](const Elf32_Sym& symbol, std::string_view symbolName){
            if (ELF32_ST_TYPE(symbol.st_info) == STT_FUNC &&
				symbol.st_shndx == p->sectionIdx &&
				symbol.st_value == symverAddr &&
				symbolName.find('@') == std::string::npos)
            {
                realSymbolName = std::string(symbolName);
                return true; // Stop iteration
            }
            return false;
        });
        
        if (realSymbolName.empty())
        {
			std::ostringstream oss;
			oss << "Could not resolve symver patch " << p->symbol 
                << " to a real symbol at address 0x" << std::hex << symverAddr;
			throw ncp::exception(oss.str());
        }

		p->symbol = realSymbolName;
    }
}

void PatchInfoAnalyzer::collectOverwriteCandidateSections(const Elf32& elf, const Elf32_Ehdr& eh, 
                                                        const Elf32_Shdr* sh_tbl, const char* str_tbl,
                                                        core::CompilationUnit* unit)
{
    Elf32::forEachSection(eh, sh_tbl, str_tbl,
    [&](std::size_t sectionIdx, const Elf32_Shdr& section, std::string_view sectionName){
        if (isValidSectionForOverwrites(sectionName, section))
        {
            auto sectionInfo = std::make_unique<SectionInfo>(SectionInfo{
                .name = std::string(sectionName),
                .size = section.sh_size,
                .unit = unit,
                .alignment = section.sh_addralign > 0 ? section.sh_addralign : 4
            });
            m_overwriteCandidateSections.push_back(std::move(sectionInfo));
        }
        return false;
    });
}

void PatchInfoAnalyzer::parseSymverSymbol(std::string_view symbolName, int sectionIdx, u32 symbolAddr, core::CompilationUnit* unit, 
                                         std::vector<GenericPatchInfo*>& patchInfoForThisObj)
{
    // Parse symver symbol: __ncp_<type>_<address>[_ov<overlay>]_<unique ids to ignore>
    std::string_view stemless = symbolName.substr(6); // Skip "__ncp_"
    
    ParsedPatchInfo parsedInfo = parseSymverPatchTypeAndAddress(stemless);
    if (!parsedInfo.isValid)
        return;

    if (parsedInfo.patchType == patch::PatchType::RtRepl)
    {
        if (ncp::Application::isVerbose())
            Log::out << OWARN << "RtRepl patches are not supported for symver patches: " << symbolName << std::endl;
        return;
    }

    if (parsedInfo.patchType == patch::PatchType::Over)
    {
        if (ncp::Application::isVerbose())
            Log::out << OWARN << "\"over\" patch must be a section type patch, not symver: " << symbolName << std::endl;
        return;
    }

    validatePatchForRegion(parsedInfo, symbolName, unit);

    // For symver patches, we initially use the symver symbol name, but we'll resolve it later
    auto patchInfoEntry = createPatchInfo(parsedInfo, symbolName, symbolAddr, sectionIdx, 0, unit, PatchSourceType::Symver);
    patchInfoForThisObj.push_back(patchInfoEntry.get());
    m_patchInfo.push_back(std::move(patchInfoEntry));
}

void PatchInfoAnalyzer::parseRegularSymbol(std::string_view symbolName, int sectionIdx, u32 symbolAddr, core::CompilationUnit* unit,
                                          std::vector<GenericPatchInfo*>& patchInfoForThisObj,
                                          const Elf32_Shdr* ncpSetSection, const Elf32_Rel* ncpSetRel,
                                          const Elf32_Sym* ncpSetRelSymTbl, std::size_t ncpSetRelCount,
                                          std::size_t ncpSetRelSymTblSize, const Elf32& elf, const Elf32_Shdr* sh_tbl)
{
    std::string_view stemless = symbolName.substr(4);
    if (stemless == "dest")
        return;

    u32 addr = symbolAddr;
    if (stemless.starts_with("set")) // requires special care because of thumb function detection
    {
        if (ncpSetSection == nullptr)
            throw ncp::exception("Found an ncp_set hook, but an \".ncp_set\" section does not exist!");

        addr = resolveNcpSetAddress(addr, elf, sh_tbl, ncpSetSection, ncpSetRel, ncpSetRelSymTbl, ncpSetRelCount, ncpSetRelSymTblSize);
    }

    ParsedPatchInfo parsedInfo = parsePatchTypeAndAddress(stemless);
    if (!parsedInfo.isValid)
        return;

    if (parsedInfo.patchType == patch::PatchType::RtRepl)
    {
        handleRtReplPatch(symbolName, unit, false);
        return;
    }

    if (parsedInfo.patchType == patch::PatchType::Over)
    {
        Log::out << OWARN << "\"over\" patch must be a section type patch: " << stemless << std::endl;
        return;
    }

    validatePatchForRegion(parsedInfo, symbolName, unit);

    auto patchInfoEntry = createPatchInfo(parsedInfo, symbolName, addr, sectionIdx, 0, unit, PatchSourceType::Label);
    patchInfoForThisObj.push_back(patchInfoEntry.get());
    m_patchInfo.push_back(std::move(patchInfoEntry));
}

void PatchInfoAnalyzer::parseSectionSymbol(std::string_view symbolName, int sectionIdx, int sectionSize,
                                          core::CompilationUnit* unit, std::vector<GenericPatchInfo*>& patchInfoForThisObj)
{
    std::string_view labelName = symbolName.substr(5); // Remove ".ncp_"

    ParsedPatchInfo parsedInfo = parsePatchTypeAndAddress(labelName);
    if (!parsedInfo.isValid)
        return;

    if (parsedInfo.patchType == patch::PatchType::RtRepl)
    {
        handleRtReplPatch(symbolName, unit, true);
        return;
    }

    if (parsedInfo.patchType == patch::PatchType::Over && sectionIdx == -1)
    {
        Log::out << OWARN << "\"over\" patch must be a section type patch: " << labelName << std::endl;
        return;
    }

    validatePatchForRegion(parsedInfo, symbolName, unit);

    auto patchInfoEntry = createPatchInfo(parsedInfo, symbolName, 0, sectionIdx, sectionSize, unit, PatchSourceType::Section);
    patchInfoForThisObj.push_back(patchInfoEntry.get());
    m_patchInfo.push_back(std::move(patchInfoEntry));
}

void PatchInfoAnalyzer::handleRtReplPatch(std::string_view symbolName, core::CompilationUnit* unit, bool isSection)
{
    if (isSection) // we do not want the labels, those are placeholders
    {
        auto rtreplPatch = std::make_unique<RtReplPatchInfo>();
        rtreplPatch->symbol = std::string(symbolName);
        rtreplPatch->unit = unit;
        m_rtreplPatches.push_back(std::move(rtreplPatch));
    }
}

void PatchInfoAnalyzer::handleNcpSetSection(const Elf32_Shdr& section, core::CompilationUnit* unit)
{
    const BuildTarget::Region* region = unit->getTargetRegion();
    int dest = region->destination;
    if (std::find(m_destWithNcpSet.begin(), m_destWithNcpSet.end(), dest) == m_destWithNcpSet.end())
        m_destWithNcpSet.emplace_back(dest);
    m_unitsWithNcpSet.emplace_back(unit);
}

u32 PatchInfoAnalyzer::resolveNcpSetAddress(u32 symbolAddr, const Elf32& elf, const Elf32_Shdr* sh_tbl,
                                           const Elf32_Shdr* ncpSetSection, const Elf32_Rel* ncpSetRel,
                                           const Elf32_Sym* ncpSetRelSymTbl, std::size_t ncpSetRelCount,
                                           std::size_t ncpSetRelSymTblSize)
{
    // Here we are just getting the address, so that we can know beforehand
    // if the function is thumb or not. The final address is obtained after linkage.
    const Elf32_Sym* symbol = reinterpret_cast<const Elf32_Sym*>(reinterpret_cast<const char*>(sh_tbl) + symbolAddr);
    auto& section = sh_tbl[symbol->st_shndx];
    auto sectionData = elf.getSection<char>(section);
    
    if (ncpSetRel == nullptr)
    {
        return Util::read<u32>(&sectionData[symbolAddr - section.sh_addr]);
    }
    else
    {
        for (int relIdx = 0; relIdx < ncpSetRelCount; relIdx++)
        {
            const Elf32_Rel& rel = ncpSetRel[relIdx];
            if (rel.r_offset == symbolAddr) // found the corresponding relocation
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
                return ncpSetRelSymTbl[symIdx].st_value;
            }
        }
        return symbolAddr;
    }
}

void PatchInfoAnalyzer::printPatchInfoForObject(const std::vector<GenericPatchInfo*>& patchInfoForThisObj) const
{
    if (ncp::Application::isVerbose())
    {
        if (patchInfoForThisObj.empty())
        {
            Log::out << ANSI_WHITE "NO PATCHES" ANSI_RESET << std::endl;
        }
        else
        {
            Log::out << ANSI_bCYAN "Object patches:" ANSI_RESET "\n"
                << ANSI_bWHITE "SRC_ADDR_OV" ANSI_RESET "  "
                << ANSI_bWHITE "DST_ADDR" ANSI_RESET "  "
                << ANSI_bWHITE "DST_ADDR_OV" ANSI_RESET "  "
                << ANSI_bWHITE "PATCH_TYPE" ANSI_RESET "  "
                << ANSI_bWHITE "SEC_IDX" ANSI_RESET "  "
                << ANSI_bWHITE "SEC_SIZE" ANSI_RESET "  "
                << ANSI_bWHITE "NCP_SET" ANSI_RESET "  "
                << ANSI_bWHITE "SRC_THUMB" ANSI_RESET "  "
                << ANSI_bWHITE "DST_THUMB" ANSI_RESET "  "
                << ANSI_bWHITE "SRC_TYPE" ANSI_RESET "  "
                << ANSI_bWHITE "SYMBOL" ANSI_RESET << std::endl;
            for (auto& p : patchInfoForThisObj)
            {
                std::string sourceTypeStr;
                switch (p->sourceType) {
                    case PatchSourceType::Section: sourceTypeStr = "section"; break;
                    case PatchSourceType::Label: sourceTypeStr = "label"; break;
                    case PatchSourceType::Symver: sourceTypeStr = "symver"; break;
                }
                
                Log::out <<
                    ANSI_YELLOW << std::setw(11) << std::dec << p->srcAddressOv << ANSI_RESET "  " <<
                    ANSI_BLUE << std::setw(8) << std::hex << p->destAddress << ANSI_RESET "  " <<
                    ANSI_YELLOW << std::setw(11) << std::dec << p->destAddressOv << ANSI_RESET "  " <<
                    ANSI_MAGENTA << std::setw(10) << PatchTypeUtils::getName(p->patchType) << ANSI_RESET "  " <<
                    ANSI_WHITE << std::setw(7) << std::dec << p->sectionIdx << ANSI_RESET "  " <<
                    ANSI_WHITE << std::setw(8) << std::dec << p->sectionSize << ANSI_RESET "  " <<
                    ANSI_GREEN << std::setw(7) << std::boolalpha << p->isNcpSet << ANSI_RESET "  " <<
                    ANSI_GREEN << std::setw(9) << std::boolalpha << p->srcThumb << ANSI_RESET "  " <<
                    ANSI_GREEN << std::setw(9) << std::boolalpha << p->destThumb << ANSI_RESET "  " <<
                    ANSI_bYELLOW << std::setw(8) << sourceTypeStr << ANSI_RESET "  " <<
                    ANSI_WHITE << p->symbol << ANSI_RESET << std::endl;
            }
        }
    }
}

void PatchInfoAnalyzer::printExternSymbols() const
{
    if (ncp::Application::isVerbose())
    {
        if (m_externSymbols.empty())
        {
            Log::out << "\n" << ANSI_bCYAN "External symbols:" ANSI_RESET " " << ANSI_WHITE "NONE" ANSI_RESET << std::endl;
        }
        else
        {
            Log::out << "\n" << ANSI_bCYAN "External symbols:" ANSI_RESET "\n";
            for (const std::string& sym : m_externSymbols)
                Log::out << ANSI_YELLOW << sym << ANSI_RESET << '\n';
            Log::out << std::flush;
        }
    }
}
