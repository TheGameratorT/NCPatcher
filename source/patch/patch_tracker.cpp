#include "patch_tracker.hpp"

#include <filesystem>
#include <iomanip>
#include <sstream>

#include "../app/application.hpp"
#include "../system/log.hpp"
#include "../system/except.hpp"
#include "../utils/util.hpp"
#include "../formats/archive.hpp"

namespace ncp::patch {

PatchTracker::PatchTracker() = default;
PatchTracker::~PatchTracker() = default;

void PatchTracker::initialize(
    const BuildTarget& target,
    const std::filesystem::path& targetWorkDir,
    core::CompilationUnitManager& compilationUnitMgr,
	DependencyResolver& dependencyResolver
)
{
    m_target = &target;
    m_targetWorkDir = &targetWorkDir;
	m_compilationUnitMgr = &compilationUnitMgr;
	m_dependencyResolver = &dependencyResolver;
}

void PatchTracker::collectPatchesFromUnits()
{
    std::filesystem::current_path(*m_targetWorkDir);
    Log::info("Getting patches from objects...");

	if (ncp::Application::isVerbose(ncp::VerboseTag::Patch))
	{
		Log::out << ANSI_bCYAN "Object patches (pre-ELF analysis):" ANSI_RESET "\n"
			<< ANSI_bYELLOW "Note: Fields marked with ? will be determined during ELF analysis" ANSI_RESET << std::endl;
	}

    for (auto& unit : m_compilationUnitMgr->getUnits())
    {
        processObjectFile(unit.get());
    }

    printExternSymbols();
}

void PatchTracker::processObjectFile(core::CompilationUnit* unit)
{
    const std::filesystem::path& objPath = unit->getObjectPath();

    if (canPrintVerboseInfo(unit))
        Log::out << ANSI_bYELLOW << objPath.string() << ANSI_RESET << std::endl;

    Elf32* elf = unit->getElf();

    const Elf32_Ehdr& eh = elf->getHeader();
    auto sh_tbl = elf->getSectionHeaderTable();
    auto str_tbl = elf->getSection<char>(sh_tbl[eh.e_shstrndx]);
    
    std::vector<PatchInfo*> objPatchInfo;

    // Process sections
    processElfSections(*elf, eh, sh_tbl, str_tbl, unit, objPatchInfo);

    // Update thumb information for patches
    updatePatchThumbInfo(*elf, eh, sh_tbl, objPatchInfo);

    // Process symbols
    processElfSymbols(*elf, eh, sh_tbl, unit, objPatchInfo);

    // Resolve symver patches to their real symbol names
    resolveSymverPatches(*elf, eh, sh_tbl, objPatchInfo);

    // Find functions that should be external (symbol and symver marked)
    for (PatchInfo* patch : objPatchInfo)
    {
        if (patch->origin == PatchOrigin::Symbol)
		{
            m_externSymbols.emplace_back(patch->symbol);
		}
		else if (patch->origin == PatchOrigin::Symver)
		{
			// Symver is stackable and uses the actual target
			// function symbol so we must filter out duplicates.
			if (std::find(m_externSymbols.begin(), m_externSymbols.end(), patch->symbol) == m_externSymbols.end())
			{
            	m_externSymbols.emplace_back(patch->symbol);
			}
		}
    }

    // Collect sections suitable for overwrites
    collectOverwriteCandidateSections(*elf, eh, sh_tbl, str_tbl, unit);

    printObjectPatchInfo(objPatchInfo, unit);
}

PatchTracker::ParsedPatchInfo PatchTracker::parsePatchTypeAndAddress(std::string_view labelName)
{
    ParsedPatchInfo info;
    
    std::size_t patchTypeNameEnd = labelName.find('_');
    if (patchTypeNameEnd == std::string::npos)
        return info;

    std::string_view patchTypeName = labelName.substr(0, patchTypeNameEnd);
    PatchType patchType = patchTypeFromString(patchTypeName);
    if (patchType == PatchType::Invalid)
    {
        Log::out << OWARN << "Found invalid patch type: " << patchTypeName << std::endl;
        return info;
    }

    info.type = patchType;

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

PatchTracker::ParsedPatchInfo PatchTracker::parseSymverPatchTypeAndAddress(std::string_view labelName)
{
    ParsedPatchInfo info;
    
    std::size_t patchTypeNameEnd = labelName.find('_');
    if (patchTypeNameEnd == std::string::npos)
        return info;

    std::string_view patchTypeName = labelName.substr(0, patchTypeNameEnd);
    PatchType patchType = patchTypeFromString(patchTypeName);
    if (patchType == PatchType::Invalid)
    {
        Log::out << OWARN << "Found invalid patch type: " << patchTypeName << std::endl;
        return info;
    }

    info.type = patchType;

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

u32 PatchTracker::parseAddress(std::string_view addressStr) const
{
    return Util::addrToInt(std::string(addressStr));
}

int PatchTracker::parseOverlay(std::string_view overlayStr) const
{
    return Util::addrToInt(std::string(overlayStr));
}

void PatchTracker::normalizePatchType(ParsedPatchInfo& info) const
{
    info.forceThumb = false;
    info.isNcpSet = false;

    switch (info.type) {
        case patch::PatchType::TJump:
            info.type = patch::PatchType::Jump;
            info.forceThumb = true;
            break;
        case patch::PatchType::TCall:
            info.type = patch::PatchType::Call;
            info.forceThumb = true;
            break;
        case patch::PatchType::THook:
            info.type = patch::PatchType::Hook;
            info.forceThumb = true;
            break;
        case patch::PatchType::SetTJump:
            info.type = patch::PatchType::SetJump;
            info.forceThumb = true;
            break;
        case patch::PatchType::SetTCall:
            info.type = patch::PatchType::SetCall;
            info.forceThumb = true;
            break;
        case patch::PatchType::SetTHook:
            info.type = patch::PatchType::SetHook;
            info.forceThumb = true;
            break;
        case patch::PatchType::SetJump:
            info.type = patch::PatchType::Jump;
            info.isNcpSet = true;
            break;
        case patch::PatchType::SetCall:
            info.type = patch::PatchType::Call;
            info.isNcpSet = true;
            break;
        case patch::PatchType::SetHook:
            info.type = patch::PatchType::Hook;
            info.isNcpSet = true;
            break;
        default:
            break;
    }

    if (info.forceThumb)
        info.destAddress |= 1;
}

std::unique_ptr<PatchInfo> PatchTracker::createPatchInfo(
	const ParsedPatchInfo& parsedInfo,
	core::CompilationUnit* unit,
    patch::PatchOrigin origin,
    std::string_view symbolName,
    u32 symbolAddr,
    int sectionIdx,
    int sectionSize
) const
{
    const BuildTarget::Region* region = unit->getTargetRegion();
    int srcAddressOv = (parsedInfo.type == patch::PatchType::Over) ? parsedInfo.destAddressOv : region->destination;

    auto patchInfoEntry = std::make_unique<PatchInfo>();
    patchInfoEntry->srcAddress = symbolAddr; // we do not yet know it, only after linkage, but we need it to resolve symver
    patchInfoEntry->srcAddressOv = srcAddressOv;
    patchInfoEntry->destAddress = (parsedInfo.destAddress & ~1);
    patchInfoEntry->destAddressOv = parsedInfo.destAddressOv;
    patchInfoEntry->type = parsedInfo.type;
    patchInfoEntry->sectionIdx = sectionIdx;
    patchInfoEntry->sectionSize = sectionSize;
    patchInfoEntry->isNcpSet = parsedInfo.isNcpSet;
    patchInfoEntry->srcThumb = bool(symbolAddr & 1);
    patchInfoEntry->destThumb = bool(parsedInfo.destAddress & 1);
    patchInfoEntry->symbol = std::string(symbolName);
    patchInfoEntry->unit = unit;
    patchInfoEntry->origin = origin;

    return patchInfoEntry;
}

void PatchTracker::validatePatchForRegion(const ParsedPatchInfo& parsedInfo, std::string_view symbolName, core::CompilationUnit* unit) const
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

bool PatchTracker::isValidSectionForOverwrites(std::string_view sectionName, const Elf32_Shdr& section) const
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

void PatchTracker::processElfSections(const Elf32& elf, const Elf32_Ehdr& eh, const Elf32_Shdr* sh_tbl, 
                                         const char* str_tbl, core::CompilationUnit* unit,
                                         std::vector<PatchInfo*>& objPatchInfo)
{
    Elf32::forEachSection(eh, sh_tbl, str_tbl,
    [&](std::size_t sectionIdx, const Elf32_Shdr& section, std::string_view sectionName){
        if (sectionName.starts_with(".ncp_"))
        {
            if (sectionName.substr(5).starts_with("set"))
            {
                // Handle ncp_set sections directly
                parseNcpSetSection(elf, eh, sh_tbl, str_tbl, sectionName, int(sectionIdx), int(section.sh_size), unit, objPatchInfo);
            }
            else
            {
                parseSectionSymbol(sectionName, int(sectionIdx), int(section.sh_size), unit, objPatchInfo);
            }
        }
        return false;
    });
}

void PatchTracker::processElfSymbols(const Elf32& elf, const Elf32_Ehdr& eh, const Elf32_Shdr* sh_tbl,
                                        core::CompilationUnit* unit, std::vector<PatchInfo*>& objPatchInfo)
{
    Elf32::forEachSymbol(elf, eh, sh_tbl,
    [&](const Elf32_Sym& symbol, std::string_view symbolName){
        if (symbolName.starts_with("ncp_"))
        {
            parseRegularSymbol(symbolName, symbol.st_shndx, symbol.st_value, unit, objPatchInfo);
        }
        else if (symbolName.starts_with("__ncp_"))
        {
            // Handle symver patches: __ncp_<type>_<address>[_ov<overlay>]_<unique ids to ignore>
            parseSymverSymbol(symbolName, symbol.st_shndx, symbol.st_value, unit, objPatchInfo);
        }
        return false;
    });
}

void PatchTracker::updatePatchThumbInfo(const Elf32& elf, const Elf32_Ehdr& eh, const Elf32_Shdr* sh_tbl,
                                            std::vector<PatchInfo*>& objPatchInfo)
{
    // Find the functions corresponding to the patch to check if they are thumb
    Elf32::forEachSymbol(elf, eh, sh_tbl,
    [&](const Elf32_Sym& symbol, std::string_view symbolName){
        if (ELF32_ST_TYPE(symbol.st_info) == STT_FUNC)
        {
			// no need to check if it is a section patch because we haven't parsed symbol patches yet
            for (PatchInfo* p : objPatchInfo)
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

void PatchTracker::resolveSymverPatches(const Elf32& elf, const Elf32_Ehdr& eh, const Elf32_Shdr* sh_tbl,
                                            std::vector<PatchInfo*>& objPatchInfo)
{
    // For each symver patch, find the real symbol that has the same address and doesn't contain '@'
    for (PatchInfo* p : objPatchInfo)
    {
        if (p->origin != PatchOrigin::Symver)
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
			oss << "Could not resolve symver patch " << p->getPrettyName()
                << " to a real symbol at address 0x" << std::hex << symverAddr;
			throw ncp::exception(oss.str());
        }

		p->symbol = realSymbolName;
    }
}

void PatchTracker::collectOverwriteCandidateSections(const Elf32& elf, const Elf32_Ehdr& eh, 
                                                        const Elf32_Shdr* sh_tbl, const char* str_tbl,
                                                        core::CompilationUnit* unit)
{
    Elf32::forEachSection(eh, sh_tbl, str_tbl,
    [&](std::size_t sectionIdx, const Elf32_Shdr& section, std::string_view sectionName){
        if (isValidSectionForOverwrites(sectionName, section))
        {
            auto sectionInfo = std::make_unique<SectionInfo>();
            sectionInfo->unit = unit;
            sectionInfo->name = std::string(sectionName);
            sectionInfo->size = section.sh_size;
            sectionInfo->alignment = section.sh_addralign > 0 ? section.sh_addralign : 4;
            m_overwriteCandidateSections.push_back(std::move(sectionInfo));
        }
        return false;
    });
}

void PatchTracker::parseSymverSymbol(std::string_view symbolName, int sectionIdx, u32 symbolAddr, core::CompilationUnit* unit, 
                                         std::vector<PatchInfo*>& objPatchInfo)
{
    // Parse symver symbol: __ncp_<type>_<address>[_ov<overlay>]_<unique ids to ignore>
    std::string_view stemless = symbolName.substr(6); // Skip "__ncp_"
    
    ParsedPatchInfo parsedInfo = parseSymverPatchTypeAndAddress(stemless);
    if (!parsedInfo.isValid)
        return;

    if (parsedInfo.type == patch::PatchType::RtRepl)
    {
        Log::out << OWARN << "RtRepl patches are not supported for symver patches: " << symbolName << std::endl;
        return;
    }

    if (parsedInfo.type == patch::PatchType::Over)
    {
        Log::out << OWARN << "\"over\" patch must be a section type patch, not symver: " << symbolName << std::endl;
        return;
    }

    validatePatchForRegion(parsedInfo, symbolName, unit);

    // For symver patches, we initially use the symver symbol name, but we'll resolve it later
    auto patchInfoEntry = createPatchInfo(parsedInfo, unit, PatchOrigin::Symver, symbolName, symbolAddr, sectionIdx, 0);
    objPatchInfo.push_back(patchInfoEntry.get());
    m_patchInfo.push_back(std::move(patchInfoEntry));
}

void PatchTracker::parseRegularSymbol(std::string_view symbolName, int sectionIdx, u32 symbolAddr, core::CompilationUnit* unit,
                                          std::vector<PatchInfo*>& objPatchInfo)
{
    std::string_view stemless = symbolName.substr(4);
    if (stemless == "dest") // TODO: check wtf this is, I have no idea. Leftover?
        return;

    // Skip ncp_set symbols since they are now handled by sections
    if (stemless.starts_with("set"))
        return;

    u32 addr = symbolAddr;

    ParsedPatchInfo parsedInfo = parsePatchTypeAndAddress(stemless);
    if (!parsedInfo.isValid)
        return;

    if (parsedInfo.type == patch::PatchType::RtRepl)
    {
        handleRtReplPatch(symbolName, unit, false);
        return;
    }

    if (parsedInfo.type == patch::PatchType::Over)
    {
        Log::out << OWARN << "\"over\" patch must be a section type patch: " << stemless << std::endl;
        return;
    }

    validatePatchForRegion(parsedInfo, symbolName, unit);

    auto patchInfoEntry = createPatchInfo(parsedInfo, unit, PatchOrigin::Symbol, symbolName, addr, sectionIdx, 0);
    objPatchInfo.push_back(patchInfoEntry.get());
    m_patchInfo.push_back(std::move(patchInfoEntry));
}

void PatchTracker::parseSectionSymbol(std::string_view symbolName, int sectionIdx, int sectionSize,
                                          core::CompilationUnit* unit, std::vector<PatchInfo*>& objPatchInfo)
{
    std::string_view labelName = symbolName.substr(5); // Remove ".ncp_"

    ParsedPatchInfo parsedInfo = parsePatchTypeAndAddress(labelName);
    if (!parsedInfo.isValid)
        return;

    if (parsedInfo.type == patch::PatchType::RtRepl)
    {
        handleRtReplPatch(symbolName, unit, true);
        return;
    }

    if (parsedInfo.type == patch::PatchType::Over && sectionIdx == -1)
    {
        Log::out << OWARN << "\"over\" patch must be a section type patch: " << labelName << std::endl;
        return;
    }

    validatePatchForRegion(parsedInfo, symbolName, unit);

    auto patchInfoEntry = createPatchInfo(parsedInfo, unit, PatchOrigin::Section, symbolName, 0, sectionIdx, sectionSize);
    objPatchInfo.push_back(patchInfoEntry.get());
    m_patchInfo.push_back(std::move(patchInfoEntry));
}

void PatchTracker::parseNcpSetSection(const Elf32& elf, const Elf32_Ehdr& eh, const Elf32_Shdr* sh_tbl,
                                          const char* str_tbl, std::string_view sectionName, int sectionIdx, int sectionSize,
                                          core::CompilationUnit* unit, std::vector<PatchInfo*>& objPatchInfo)
{
    // Validate section size (should be exactly 4 bytes for a pointer)
    if (sectionSize != 4)
    {
        Log::out << OWARN << "ncp_set section " << sectionName << " should be exactly 4 bytes, but is " 
                 << sectionSize << " bytes." << std::endl;
        return;
    }

    // Parse section name: .ncp_set<opcode>_<address>[_ov<overlay>]
    std::string_view labelName = sectionName.substr(5); // Remove ".ncp_"

    ParsedPatchInfo parsedInfo = parsePatchTypeAndAddress(labelName);
    if (!parsedInfo.isValid)
        return;

    // Force this to be an ncp_set patch
    parsedInfo.isNcpSet = true;

    if (parsedInfo.type == patch::PatchType::RtRepl)
    {
        Log::out << OWARN << "\"rtrepl\" patch type is not supported for ncp_set sections: " << labelName << std::endl;
        return;
    }

    validatePatchForRegion(parsedInfo, sectionName, unit);

    // Determine srcThumb by analyzing relocations for this section
    bool srcThumb = false;
    std::string relocSectionName = ".rel" + std::string(sectionName);
    
    // Find the relocation section for this ncp_set section
    Elf32::forEachRelocation(elf, eh, sh_tbl,
    [&](const Elf32_Rel& relocation, std::string_view relocSecName, std::string_view targetSecName) -> bool {
        if (relocSecName == relocSectionName && targetSecName == sectionName)
        {
            // Get the symbol table for this relocation section
            const Elf32_Shdr* relocSection = nullptr;
            for (std::size_t i = 0; i < eh.e_shnum; i++)
            {
                if (std::string_view(&str_tbl[sh_tbl[i].sh_name]) == relocSecName)
                {
                    relocSection = &sh_tbl[i];
                    break;
                }
            }
            
            if (relocSection && relocSection->sh_link < eh.e_shnum)
            {
                auto sym_tbl = elf.getSection<Elf32_Sym>(sh_tbl[relocSection->sh_link]);
                auto sym_str_tbl = elf.getSection<char>(sh_tbl[sh_tbl[relocSection->sh_link].sh_link]);
                
                u32 symIdx = ELF32_R_SYM(relocation.r_info);
                if (symIdx < sh_tbl[relocSection->sh_link].sh_size / sizeof(Elf32_Sym))
                {
                    const Elf32_Sym& referencedSymbol = sym_tbl[symIdx];
                    std::string referencedSymbolName;
                    
                    if (referencedSymbol.st_name == 0 && ELF32_ST_TYPE(referencedSymbol.st_info) == STT_SECTION)
                    {
                        // This is a section symbol - we can't determine thumb bit from sections
                        return false;
                    }
                    else
                    {
                        // This is a named symbol
                        referencedSymbolName = std::string(&sym_str_tbl[referencedSymbol.st_name]);
                        
                        // First check if symbol is defined locally with thumb bit
                        if (referencedSymbol.st_shndx != SHN_UNDEF)
                        {
                            srcThumb = bool(referencedSymbol.st_value & 1);
                        }
                        else
                        {
                            // Symbol is external, use DependencyResolver to find it
                            const auto* symbolInfo = m_dependencyResolver->findSymbol(referencedSymbolName);
                            if (symbolInfo)
                            {
                                srcThumb = bool(symbolInfo->address & 1);
                            }
                        }
                        
                        return true; // Found the relocation we were looking for
                    }
                }
            }
        }
        return false;
    });

    auto patchInfoEntry = createPatchInfo(parsedInfo, unit, PatchOrigin::Section, sectionName, 0, sectionIdx, sectionSize);
    patchInfoEntry->srcThumb = srcThumb;
    objPatchInfo.push_back(patchInfoEntry.get());
    m_patchInfo.push_back(std::move(patchInfoEntry));
}

void PatchTracker::handleRtReplPatch(std::string_view symbolName, core::CompilationUnit* unit, bool isSection)
{
    if (isSection) // we do not want the labels, those are placeholders
    {
        auto rtreplPatch = std::make_unique<PatchInfo>();
        rtreplPatch->unit = unit;
        rtreplPatch->symbol = std::string(symbolName);
        m_rtreplPatches.push_back(std::move(rtreplPatch));
    }
}

void PatchTracker::finalizePatchesWithElfData(const Elf32& elf)
{
    Log::info("Getting patches from elf...");

    const Elf32_Ehdr& eh = elf.getHeader();
    auto sh_tbl = elf.getSectionHeaderTable();
    auto str_tbl = elf.getSection<char>(sh_tbl[eh.e_shstrndx]);

    // Update the patch info with new values
    Elf32::forEachSymbol(elf, eh, sh_tbl,
    [&](const Elf32_Sym& symbol, std::string_view symbolName){
        for (auto& patch : m_patchInfo)
        {
            if (patch->origin == PatchOrigin::Section)
            {
				if (patch->isNcpSet)
				{
					if (patch->symbol == symbolName)
					{
						patch->sectionIdx = symbol.st_shndx;
					}
				}
				else
				{
					std::string_view nameAsLabel = std::string_view(patch->symbol).substr(1);
					if (nameAsLabel == symbolName)
					{
						patch->srcAddress = symbol.st_value;
						patch->sectionIdx = symbol.st_shndx;
						patch->symbol = nameAsLabel;
					}
				}
            }
            else
            {
                // This must run before fetching ncp_set section, otherwise ncp_set srcAddr will be overwritten
                if (patch->symbol == symbolName)
                {
                    patch->srcAddress = symbol.st_value & ~1;
                    patch->sectionIdx = symbol.st_shndx;
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
            auto info = std::make_unique<AutogenDataInfo>();
            info->address = symbol.st_value;
            info->curAddress = symbol.st_value;
            m_autogenDataInfoForDest.emplace(srcAddrOv, std::move(info));
        }
        return false;
    });

    Elf32::forEachSection(eh, sh_tbl, str_tbl,
    [&](std::size_t sectionIdx, const Elf32_Shdr& section, std::string_view sectionName){
        for (auto& patch : m_patchInfo)
        {
            if (patch->type == PatchType::Over)
            {
                if (patch->symbol == sectionName)
                {
                    patch->srcAddress = section.sh_addr; // should be the same as the destination
                    patch->sectionIdx = int(sectionIdx);
                }
            }
        }
        if (sectionName.starts_with(".ncp_set"))
        {
            // Handle ncp_set sections directly - each section corresponds to a specific patch
            const char* sectionData = elf.getSection<char>(section);
            
            // Find the patch that corresponds to this section
            for (auto& patch : m_patchInfo)
            {
                if (patch->isNcpSet && patch->symbol == sectionName)
                {
                    if (section.sh_size != 4)
                    {
                        std::ostringstream oss;
                        oss << "ncp_set section " << OSTR(sectionName) << " should be exactly 4 bytes, but is " << section.sh_size << " bytes.";
                        throw ncp::exception(oss.str());
                    }
                    
                    // Read the function pointer from the section data
                    // ncp_set comes with the THUMB bit, we must clear it!
                    patch->srcAddress = Util::read<u32>(&sectionData[0]) & ~1;
                    // The srcThumb information was already determined during object processing
                    // in ObjectAnalyzer::parseNcpSetSection, so we don't update it here
                }
            }
        }
        return false;
    });
    
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
        for (const auto& p : m_patchInfo)
        {
			if (ncp::Application::isVerbose(ncp::VerboseTag::NoLib) && p->unit->getType() == core::CompilationUnitType::LibraryFile)
				continue;

			// TODO: marks

            Log::out <<
                ANSI_CYAN << std::setw(10) << Util::intToAddr(p->srcAddress, 8) << "*";
            Log::out << ANSI_RESET " " <<
                ANSI_YELLOW << std::setw(11) << std::dec << p->srcAddressOv << ANSI_RESET "  " <<
                ANSI_BLUE << std::setw(8) << Util::intToAddr(p->destAddress, 8) << ANSI_RESET "  " <<
                ANSI_YELLOW << std::setw(11) << std::dec << p->destAddressOv << ANSI_RESET "  " <<
                ANSI_MAGENTA << std::setw(10) << toString(p->type) << ANSI_RESET "  " <<
                ANSI_WHITE << std::setw(8) << std::dec << p->sectionIdx << "*";
            Log::out << ANSI_RESET " " <<
                ANSI_WHITE << std::setw(8) << std::dec << p->sectionSize << ANSI_RESET "  " <<
                ANSI_GREEN << std::setw(7) << std::boolalpha << p->isNcpSet << ANSI_RESET "  " <<
                ANSI_GREEN << std::setw(9) << std::boolalpha << p->srcThumb;
            Log::out << ANSI_RESET "  " <<
                ANSI_GREEN << std::setw(9) << std::boolalpha << p->destThumb << ANSI_RESET "  " <<
                ANSI_bYELLOW << std::setw(11) << toString(p->origin) << ANSI_RESET "  " <<
                ANSI_WHITE << p->symbol;
            Log::out << ANSI_RESET << std::endl;
        }
    }

	fetchNewcodeInfo(elf, eh, sh_tbl, str_tbl);
}

void PatchTracker::checkForOverlappingPatches()
{
    // Check if any overlapping patches exist
    bool foundOverlapping = false;
    for (std::size_t i = 0; i < m_patchInfo.size(); i++)
    {
        auto& a = m_patchInfo[i];
        for (std::size_t j = i + 1; j < m_patchInfo.size(); j++)
        {
            auto& b = m_patchInfo[j];
            if (a->destAddressOv != b->destAddressOv)
                continue;
            u32 aSz = a->getOverwriteAmount();
            u32 bSz = b->getOverwriteAmount();
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
}

void PatchTracker::fetchNewcodeInfo(const Elf32& elf, const Elf32_Ehdr& eh, const Elf32_Shdr* sh_tbl, const char* str_tbl)
{
    Elf32::forEachSection(eh, sh_tbl, str_tbl,
    [&](std::size_t sectionIdx, const Elf32_Shdr& section, std::string_view sectionName){
        auto insertSection = [&](int dest, bool isBss){
            auto& newcodeInfo = m_newcodeInfoForDest[dest];
            if (newcodeInfo == nullptr)
                newcodeInfo = std::make_unique<NewcodeInfo>();

            (isBss ? newcodeInfo->bssData : newcodeInfo->binData) = elf.getSection<u8>(section);
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
        for (const auto& [dest, newcodeInfo] : m_newcodeInfoForDest)
        {
            Log::out <<
                ANSI_YELLOW << std::setw(8) << std::left << (dest == -1 ? "ARM" : ("OV" + std::to_string(dest))) << ANSI_RESET << std::right <<
                ANSI_CYAN << std::setw(9) << std::dec << newcodeInfo->binSize << ANSI_RESET "    " <<
                ANSI_CYAN << std::setw(8) << std::dec << newcodeInfo->bssSize << ANSI_RESET << std::endl;
        }
    }	
}

void PatchTracker::printObjectPatchInfo(const std::vector<PatchInfo*>& objPatchInfo, core::CompilationUnit* unit) const
{
    if (!canPrintVerboseInfo(unit))
		return;

	if (objPatchInfo.empty())
	{
		Log::out << ANSI_WHITE "NO PATCHES" ANSI_RESET << std::endl;
	}
	else
	{
		Log::out
			<< ANSI_bWHITE "SRC_ADDR_OV" ANSI_RESET "    "
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
		for (auto& patch : objPatchInfo)
		{
			Log::out <<
				ANSI_YELLOW << std::setw(11) << std::dec << patch->srcAddressOv << ANSI_RESET "  " <<
				ANSI_BLUE << std::setw(8) << Util::intToAddr(patch->destAddress, 8) << ANSI_RESET "  " <<
				ANSI_YELLOW << std::setw(11) << std::dec << patch->destAddressOv << ANSI_RESET "  " <<
				ANSI_MAGENTA << std::setw(10) << toString(patch->type) << ANSI_RESET "  " <<
				ANSI_WHITE << std::setw(7) << std::dec << patch->sectionIdx << "?";
			Log::out << ANSI_RESET " " <<
				ANSI_WHITE << std::setw(8) << std::dec << patch->sectionSize << ANSI_RESET "  " <<
				ANSI_GREEN << std::setw(7) << std::boolalpha << patch->isNcpSet << ANSI_RESET "  " <<
				ANSI_GREEN << std::setw(9) << std::boolalpha << patch->srcThumb;
			Log::out << ANSI_RESET "  " <<
				ANSI_GREEN << std::setw(9) << std::boolalpha << patch->destThumb << ANSI_RESET "  " <<
				ANSI_bYELLOW << std::setw(8) << toString(patch->origin) << ANSI_RESET "  " <<
				ANSI_WHITE << patch->symbol;
			Log::out << ANSI_RESET << std::endl;
		}
	}
}

void PatchTracker::printExternSymbols() const
{
    if (ncp::Application::isVerbose(ncp::VerboseTag::Patch))
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

bool PatchTracker::canPrintVerboseInfo(core::CompilationUnit* unit) const
{
    if (!ncp::Application::isVerbose(ncp::VerboseTag::Patch) ||
		(ncp::Application::isVerbose(ncp::VerboseTag::NoLib) && unit->getType() == core::CompilationUnitType::LibraryFile))
		return false;
	return true;
}

}
