#pragma once

#include <vector>
#include <memory>
#include <string>
#include <functional>

#include "../utils/types.hpp"
#include "../formats/elf.hpp"
#include "../core/compilation_unit_manager.hpp"
#include "../config/buildtarget.hpp"
#include "types.hpp"

// Use the centralized types from patch::types
using patch::GenericPatchInfo;
using patch::RtReplPatchInfo;
using patch::SectionInfo;

class PatchInfoAnalyzer
{
public:
    PatchInfoAnalyzer();
    ~PatchInfoAnalyzer();

    void initialize(
        const BuildTarget& target,
        const std::filesystem::path& targetWorkDir,
		core::CompilationUnitManager& compilationUnitMgr
    );

    void gatherInfoFromObjects();

    // Getters
    const std::vector<std::unique_ptr<GenericPatchInfo>>& getPatchInfo() const { return m_patchInfo; }
    const std::vector<std::unique_ptr<RtReplPatchInfo>>& getRtreplPatches() const { return m_rtreplPatches; }
    const std::vector<std::string>& getExternSymbols() const { return m_externSymbols; }
    const std::vector<std::unique_ptr<SectionInfo>>& getOverwriteCandidateSections() const { return m_overwriteCandidateSections; }
    const std::vector<int>& getDestWithNcpSet() const { return m_destWithNcpSet; }
    const core::CompilationUnitPtrCollection& getUnitsWithNcpSet() const { return m_unitsWithNcpSet; }

    // Move semantics for transfer to other components
    std::vector<std::unique_ptr<GenericPatchInfo>> takePatchInfo() { return std::move(m_patchInfo); }
    std::vector<std::unique_ptr<RtReplPatchInfo>> takeRtreplPatches() { return std::move(m_rtreplPatches); }
    std::vector<std::string> takeExternSymbols() { return std::move(m_externSymbols); }
    std::vector<std::unique_ptr<SectionInfo>> takeOverwriteCandidateSections() { return std::move(m_overwriteCandidateSections); }

private:
    struct ParsedPatchInfo {
        std::size_t patchType;
        u32 destAddress;
        int destAddressOv;
        bool isNcpSet;
        bool forceThumb;
        bool isValid;
        
        ParsedPatchInfo() : patchType(0), destAddress(0), destAddressOv(-1), 
                           isNcpSet(false), forceThumb(false), isValid(false) {}
    };

    const BuildTarget* m_target;
    const std::filesystem::path* m_targetWorkDir;
    core::CompilationUnitManager* m_compilationUnitMgr;

    std::vector<std::unique_ptr<GenericPatchInfo>> m_patchInfo;
    std::vector<std::unique_ptr<RtReplPatchInfo>> m_rtreplPatches;
    std::vector<int> m_destWithNcpSet;
    core::CompilationUnitPtrCollection m_unitsWithNcpSet;
    std::vector<std::string> m_externSymbols;
    std::vector<std::unique_ptr<SectionInfo>> m_overwriteCandidateSections;

    // ELF loading helpers
    bool loadElfFromArchive(Elf32& elf, const std::filesystem::path& archivePath, const std::string& memberName);
    bool loadElfFromPath(Elf32& elf, const std::filesystem::path& objPath);

    // Patch parsing helpers
    ParsedPatchInfo parsePatchTypeAndAddress(std::string_view labelName);
    ParsedPatchInfo parseSymverPatchTypeAndAddress(std::string_view labelName);
    u32 parseAddress(std::string_view addressStr) const;
    int parseOverlay(std::string_view overlayStr) const;
    void normalizePatchType(ParsedPatchInfo& info) const;

    // Patch creation helpers
    std::unique_ptr<GenericPatchInfo> createPatchInfo(
        const ParsedPatchInfo& parsedInfo,
        std::string_view symbolName,
        u32 symbolAddr,
        int sectionIdx,
        int sectionSize,
        core::CompilationUnit* unit,
        patch::PatchSourceType sourceType
    ) const;

    // Validation helpers
    void validatePatchForRegion(const ParsedPatchInfo& parsedInfo, std::string_view symbolName, core::CompilationUnit* unit) const;
    bool isValidSectionForOverwrites(std::string_view sectionName, const Elf32_Shdr& section) const;

    // Symbol and section processing
    void processObjectFile(core::CompilationUnit* unit);
    void processElfSections(const Elf32& elf, const Elf32_Ehdr& eh, const Elf32_Shdr* sh_tbl, 
                           const char* str_tbl, core::CompilationUnit* unit,
                           std::vector<GenericPatchInfo*>& patchInfoForThisObj,
                           const Elf32_Shdr*& ncpSetSection, const Elf32_Rel*& ncpSetRel,
                           const Elf32_Sym*& ncpSetRelSymTbl, std::size_t& ncpSetRelCount, 
                           std::size_t& ncpSetRelSymTblSize);
    void processElfSymbols(const Elf32& elf, const Elf32_Ehdr& eh, const Elf32_Shdr* sh_tbl,
                          core::CompilationUnit* unit, std::vector<GenericPatchInfo*>& patchInfoForThisObj,
                          const Elf32_Shdr* ncpSetSection, const Elf32_Rel* ncpSetRel,
                          const Elf32_Sym* ncpSetRelSymTbl, std::size_t ncpSetRelCount,
                          std::size_t ncpSetRelSymTblSize);
    void updatePatchThumbInfo(const Elf32& elf, const Elf32_Ehdr& eh, const Elf32_Shdr* sh_tbl,
                             std::vector<GenericPatchInfo*>& patchInfoForThisObj);
    void resolveSymverPatches(const Elf32& elf, const Elf32_Ehdr& eh, const Elf32_Shdr* sh_tbl,
                             std::vector<GenericPatchInfo*>& patchInfoForThisObj);
    void collectOverwriteCandidateSections(const Elf32& elf, const Elf32_Ehdr& eh, 
                                         const Elf32_Shdr* sh_tbl, const char* str_tbl,
                                         core::CompilationUnit* unit);

    // Symbol parsing methods
    void parseSymverSymbol(std::string_view symbolName, int sectionIdx, u32 symbolAddr, core::CompilationUnit* unit, 
                          std::vector<GenericPatchInfo*>& patchInfoForThisObj);
    void parseRegularSymbol(std::string_view symbolName, int sectionIdx, u32 symbolAddr, core::CompilationUnit* unit,
                           std::vector<GenericPatchInfo*>& patchInfoForThisObj,
                           const Elf32_Shdr* ncpSetSection, const Elf32_Rel* ncpSetRel,
                           const Elf32_Sym* ncpSetRelSymTbl, std::size_t ncpSetRelCount,
                           std::size_t ncpSetRelSymTblSize, const Elf32& elf, const Elf32_Shdr* sh_tbl);
    void parseSectionSymbol(std::string_view symbolName, int sectionIdx, int sectionSize,
                           core::CompilationUnit* unit, std::vector<GenericPatchInfo*>& patchInfoForThisObj);

    // Special handling methods
    void handleRtReplPatch(std::string_view symbolName, core::CompilationUnit* unit, bool isSection);
    void handleNcpSetSection(const Elf32_Shdr& section, core::CompilationUnit* unit);
    u32 resolveNcpSetAddress(u32 symbolAddr, const Elf32& elf, const Elf32_Shdr* sh_tbl,
                            const Elf32_Shdr* ncpSetSection, const Elf32_Rel* ncpSetRel,
                            const Elf32_Sym* ncpSetRelSymTbl, std::size_t ncpSetRelCount,
                            std::size_t ncpSetRelSymTblSize);

    // Debugging and output helpers
    void printPatchInfoForObject(const std::vector<GenericPatchInfo*>& patchInfoForThisObj) const;
    void printExternSymbols() const;
};
