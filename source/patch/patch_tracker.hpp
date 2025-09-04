#pragma once

#include <vector>
#include <memory>
#include <string>
#include <functional>

#include "../utils/types.hpp"
#include "../formats/elf.hpp"
#include "../core/compilation_unit_manager.hpp"
#include "../config/buildtarget.hpp"
#include "dependency_resolver.hpp"
#include "types.hpp"

namespace ncp::patch {

class PatchTracker
{
public:
    PatchTracker();
    ~PatchTracker();

    void initialize(
        const BuildTarget& target,
        const std::filesystem::path& targetWorkDir,
		core::CompilationUnitManager& compilationUnitMgr,
		DependencyResolver& dependencyResolver
    );

    void collectPatchesFromUnits();
    void finalizePatchesWithElfData(const Elf32& elf);
	
	void checkForOverlappingPatches();

    // Getters
    const std::vector<std::unique_ptr<PatchInfo>>& getPatchInfo() const { return m_patchInfo; }
    const std::vector<std::unique_ptr<PatchInfo>>& getRtreplPatches() const { return m_rtreplPatches; }
    const std::vector<std::string>& getExternSymbols() const { return m_externSymbols; }
    std::vector<std::unique_ptr<SectionInfo>>& getOverwriteCandidateSections() { return m_overwriteCandidateSections; }
    const std::unordered_map<int, std::unique_ptr<NewcodeInfo>>& getNewcodeInfoForDest() const { return m_newcodeInfoForDest; }
    const std::unordered_map<int, std::unique_ptr<AutogenDataInfo>>& getAutogenDataInfoForDest() const { return m_autogenDataInfoForDest; }

private:
    struct ParsedPatchInfo {
        PatchType type;
        u32 destAddress;
        int destAddressOv;
        bool isNcpSet;
        bool forceThumb;
        bool isValid;
        
        ParsedPatchInfo() : type(PatchType::Jump), destAddress(0), destAddressOv(-1), 
                           isNcpSet(false), forceThumb(false), isValid(false) {}
    };

    const BuildTarget* m_target;
    const std::filesystem::path* m_targetWorkDir;
    core::CompilationUnitManager* m_compilationUnitMgr;
	DependencyResolver* m_dependencyResolver;

    std::vector<std::unique_ptr<PatchInfo>> m_patchInfo;
    std::vector<std::unique_ptr<PatchInfo>> m_rtreplPatches;
    std::vector<std::string> m_externSymbols;
    std::vector<std::unique_ptr<SectionInfo>> m_overwriteCandidateSections;
    std::unordered_map<int, std::unique_ptr<NewcodeInfo>> m_newcodeInfoForDest;
    std::unordered_map<int, std::unique_ptr<AutogenDataInfo>> m_autogenDataInfoForDest;

    // Patch parsing helpers
    ParsedPatchInfo parsePatchTypeAndAddress(std::string_view labelName);
    ParsedPatchInfo parseSymverPatchTypeAndAddress(std::string_view labelName);
    u32 parseAddress(std::string_view addressStr) const;
    int parseOverlay(std::string_view overlayStr) const;
    void normalizePatchType(ParsedPatchInfo& info) const;

    // Patch creation helpers
    std::unique_ptr<PatchInfo> createPatchInfo(
        const ParsedPatchInfo& parsedInfo,
        core::CompilationUnit* unit,
        PatchOrigin origin,
        std::string_view symbolName,
        u32 symbolAddr,
        int sectionIdx,
        int sectionSize
    ) const;

    // Validation helpers
    void validatePatchForRegion(const ParsedPatchInfo& parsedInfo, std::string_view symbolName, core::CompilationUnit* unit) const;
    bool isValidSectionForOverwrites(std::string_view sectionName, const Elf32_Shdr& section) const;

    // Symbol and section processing
    void processObjectFile(core::CompilationUnit* unit);
    void processElfSections(const Elf32& elf, const Elf32_Ehdr& eh, const Elf32_Shdr* sh_tbl, 
                           const char* str_tbl, core::CompilationUnit* unit,
                           std::vector<PatchInfo*>& objPatchInfo);
    void processElfSymbols(const Elf32& elf, const Elf32_Ehdr& eh, const Elf32_Shdr* sh_tbl,
                          core::CompilationUnit* unit, std::vector<PatchInfo*>& objPatchInfo);
    void updatePatchThumbInfo(const Elf32& elf, const Elf32_Ehdr& eh, const Elf32_Shdr* sh_tbl,
                             std::vector<PatchInfo*>& objPatchInfo);
    void resolveSymverPatches(const Elf32& elf, const Elf32_Ehdr& eh, const Elf32_Shdr* sh_tbl,
                             std::vector<PatchInfo*>& objPatchInfo);
    void collectOverwriteCandidateSections(const Elf32& elf, const Elf32_Ehdr& eh, 
                                         const Elf32_Shdr* sh_tbl, const char* str_tbl,
                                         core::CompilationUnit* unit);

    // Symbol parsing methods
    void parseSymverSymbol(std::string_view symbolName, int sectionIdx, u32 symbolAddr, core::CompilationUnit* unit, 
                          std::vector<PatchInfo*>& objPatchInfo);
    void parseRegularSymbol(std::string_view symbolName, int sectionIdx, u32 symbolAddr, core::CompilationUnit* unit,
                           std::vector<PatchInfo*>& objPatchInfo);
    void parseSectionSymbol(std::string_view symbolName, int sectionIdx, int sectionSize,
                           core::CompilationUnit* unit, std::vector<PatchInfo*>& objPatchInfo);
    void parseNcpSetSection(const Elf32& elf, const Elf32_Ehdr& eh, const Elf32_Shdr* sh_tbl,
                           const char* str_tbl, std::string_view sectionName, int sectionIdx, int sectionSize,
                           core::CompilationUnit* unit, std::vector<PatchInfo*>& objPatchInfo);

    // Special handling methods
    void handleRtReplPatch(std::string_view symbolName, core::CompilationUnit* unit, bool isSection);

	void fetchNewcodeInfo(const Elf32& elf, const Elf32_Ehdr& eh, const Elf32_Shdr* sh_tbl, const char* str_tbl);

    // Debugging and output helpers
    void printObjectPatchInfo(const std::vector<PatchInfo*>& objPatchInfo, core::CompilationUnit* unit) const;
    void printExternSymbols() const;
	bool canPrintVerboseInfo(core::CompilationUnit* unit) const;
};

} // namespace ncp::patch
