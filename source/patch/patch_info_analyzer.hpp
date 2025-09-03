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

    struct RelocationInfo {
        const Elf32_Rel* relocations;
        std::size_t relocationCount;
        const Elf32_Sym* symbolTable;
        std::size_t symbolTableSize;
        
        RelocationInfo() : relocations(nullptr), relocationCount(0), 
                          symbolTable(nullptr), symbolTableSize(0) {}
    };

    const BuildTarget* m_target;
    const std::filesystem::path* m_targetWorkDir;
    core::CompilationUnitManager* m_compilationUnitMgr;

    std::vector<std::unique_ptr<GenericPatchInfo>> m_patchInfo;
    std::vector<std::unique_ptr<RtReplPatchInfo>> m_rtreplPatches;
    std::vector<std::string> m_externSymbols;
    std::vector<std::unique_ptr<SectionInfo>> m_overwriteCandidateSections;

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
                           std::vector<GenericPatchInfo*>& patchInfoForThisObj);
    void processElfSymbols(const Elf32& elf, const Elf32_Ehdr& eh, const Elf32_Shdr* sh_tbl,
                          core::CompilationUnit* unit, std::vector<GenericPatchInfo*>& patchInfoForThisObj);
    void updatePatchThumbInfo(const Elf32& elf, const Elf32_Ehdr& eh, const Elf32_Shdr* sh_tbl,
                             std::vector<GenericPatchInfo*>& patchInfoForThisObj);
    void resolveSymverPatches(const Elf32& elf, const Elf32_Ehdr& eh, const Elf32_Shdr* sh_tbl,
                             std::vector<GenericPatchInfo*>& patchInfoForThisObj);
    void collectOverwriteCandidateSections(const Elf32& elf, const Elf32_Ehdr& eh, 
                                         const Elf32_Shdr* sh_tbl, const char* str_tbl,
                                         core::CompilationUnit* unit);
    
    // Relocation processing
    RelocationInfo findRelocationInfo(const Elf32& elf, const Elf32_Ehdr& eh, const Elf32_Shdr* sh_tbl,
                                     const char* str_tbl, std::string_view targetSectionName);
    bool extractSrcThumbFromRelocation(const Elf32& elf, const Elf32_Shdr& section, u32 sectionOffset,
                                      const RelocationInfo& relocInfo, bool& srcThumb);

    // Symbol parsing methods
    void parseSymverSymbol(std::string_view symbolName, int sectionIdx, u32 symbolAddr, core::CompilationUnit* unit, 
                          std::vector<GenericPatchInfo*>& patchInfoForThisObj);
    void parseRegularSymbol(std::string_view symbolName, int sectionIdx, u32 symbolAddr, core::CompilationUnit* unit,
                           std::vector<GenericPatchInfo*>& patchInfoForThisObj);
    void parseSectionSymbol(std::string_view symbolName, int sectionIdx, int sectionSize,
                           core::CompilationUnit* unit, std::vector<GenericPatchInfo*>& patchInfoForThisObj);
    void parseNcpSetSection(const Elf32& elf, const Elf32_Ehdr& eh, const Elf32_Shdr* sh_tbl,
                           const char* str_tbl, std::string_view sectionName, int sectionIdx, int sectionSize,
                           core::CompilationUnit* unit, std::vector<GenericPatchInfo*>& patchInfoForThisObj);

    // Special handling methods
    void handleRtReplPatch(std::string_view symbolName, core::CompilationUnit* unit, bool isSection);

    // Debugging and output helpers
    void printPatchInfoForObject(const std::vector<GenericPatchInfo*>& patchInfoForThisObj, core::CompilationUnit* unit) const;
    void printExternSymbols() const;
	bool canPrintVerboseInfo(core::CompilationUnit* unit) const;
};
