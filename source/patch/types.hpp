#pragma once

#include <vector>
#include <memory>
#include <string>
#include <unordered_set>
#include <unordered_map>

#include "../utils/types.hpp"
#include "../formats/elf.hpp"
#include "../core/compilation_unit.hpp"

// Forward declarations for component classes
class PatchInfoAnalyzer;
class LibraryAnalyzer;
class SectionUsageAnalyzer;
class OverwriteRegionManager;
class ElfAnalyzer;

namespace patch {

/**
 * Base information about a section from an ELF file
 */
struct SectionInfo
{
    std::string name;
    std::size_t size;
    core::CompilationUnit* unit;
    u32 alignment;
    u32 address = 0;
    const u8* data = nullptr;
    int destination = -1;
};

/**
 * Reference to a symbol that can be either a named symbol or a section
 */
struct ReferencedSymbol
{
    std::string name;
    bool isSection;
    
    ReferencedSymbol(const std::string& n, bool section) : name(n), isSection(section) {}
    
    bool operator==(const ReferencedSymbol& other) const {
        return name == other.name && isSection == other.isSection;
    }
};

/**
 * Hash function for ReferencedSymbol to use in unordered_set
 */
struct ReferencedSymbolHash
{
    std::size_t operator()(const ReferencedSymbol& rs) const {
        return std::hash<std::string>{}(rs.name) ^ (std::hash<bool>{}(rs.isSection) << 1);
    }
};

/**
 * Extended section information with usage tracking for analysis
 */
struct SectionUsageInfo
{
    std::string name;
    std::size_t size;
    core::CompilationUnit* unit;
    u32 alignment;
    bool isReferenced = false;
    bool isEntryPoint = false;
    std::unordered_set<ReferencedSymbol, ReferencedSymbolHash> referencedSymbols;
};

/**
 * Information about a symbol from an ELF file
 */
struct SymbolInfo
{
    std::string name;
    std::string sectionName;
    core::CompilationUnit* unit;
    bool isFunction = false;
    bool isGlobal = false;
    bool isWeak = false;
    u32 address = 0;
    u32 size = 0;
};

/**
 * Information about an overwrite region where sections can be placed
 */
struct OverwriteRegionInfo
{
    u32 startAddress;
    u32 endAddress;
    int destination;
    std::vector<SectionInfo*> assignedSections;
    std::vector<struct GenericPatchInfo*> sectionPatches;
    u32 usedSize = 0;
    std::string memName;
    int sectionIdx = -1;
    int sectionSize = 0;
};

/**
 * Base patch information structure
 */
struct PatchInfo
{
    std::string symbol;
    core::CompilationUnit* unit;
    u32 srcAddress = 0;
    int srcAddressOv = -1;
    u32 destAddress = 0;
    int destAddressOv = -1;
    std::size_t patchType = 0;
    bool srcThumb = false;
    bool destThumb = false;
};

/**
 * Enumeration for patch source types
 */
enum class PatchSourceType {
    Section,    // Section-based patch (sectionIdx >= 0)
    Label,      // Label-based patch (sectionIdx == -1, traditional)
    Symver      // Symbol versioned patch (sectionIdx == -1, new type)
};

const char* toString(PatchSourceType patchSourceType);

/**
 * Generic patch information with section support
 * 
 * Fields populated during initial analysis (PatchInfoAnalyzer):
 * - symbol, destAddress, destAddressOv, patchType, srcAddressOv
 * - srcThumb, destThumb (for most patches)
 * - isNcpSet, sourceType, sectionSize
 * 
 * Fields populated/updated during ELF analysis (ElfAnalyzer):
 * - srcAddress (always updated from symbol/section data)
 * - sectionIdx (always updated from ELF symbol/section info)
 * - srcThumb (for ncp_set patches only - read from function pointer LSB)
 * - symbol (for section-type patches - dot prefix removed)
 */
struct GenericPatchInfo : public PatchInfo
{
    int sectionIdx = -1;
    int sectionSize = 0;
    bool isNcpSet = false;
    PatchSourceType sourceType = PatchSourceType::Section;
    
    /**
     * Formats patch information as "ncp_type(address[, overlay])" format
     * @return String in the format like "ncp_jump(0x02000000)" or "ncp_hook(0x02000000, ov5)"
     */
    std::string formatPatchDescriptor() const;
};

/**
 * Runtime replacement patch information
 */
struct RtReplPatchInfo : public PatchInfo
{
    // Inherits all fields from PatchInfo
    // Additional fields can be added here if needed
};

/**
 * Information about newcode data for a destination
 */
struct NewcodePatch
{
    const u8* binData = nullptr;
    const u8* bssData = nullptr;
    std::size_t binSize = 0;
    std::size_t binAlign = 4;
    std::size_t bssSize = 0;
    std::size_t bssAlign = 4;
};

/**
 * Information about auto-generated data for a destination
 */
struct AutogenDataInfo
{
    u32 address = 0;
    u32 curAddress = 0;
    std::vector<u8> data;
};

/**
 * Context information for patch operations
 */
struct PatchOperationContext
{
    const std::vector<std::unique_ptr<GenericPatchInfo>>* patchInfo;
    const std::vector<std::unique_ptr<RtReplPatchInfo>>* rtreplPatches;
    const std::unordered_map<int, std::unique_ptr<NewcodePatch>>* newcodeDataForDest;
    const std::unordered_map<int, std::unique_ptr<AutogenDataInfo>>* autogenDataInfoForDest;
    const std::unordered_map<int, u32>* newcodeAddrForDest;
    const void* sectionHeaderTable; // Elf32_Shdr*, but avoiding ELF include here
    
    PatchOperationContext(
        const std::vector<std::unique_ptr<GenericPatchInfo>>& patches,
        const std::vector<std::unique_ptr<RtReplPatchInfo>>& rtreplPatchesRef,
        const std::unordered_map<int, std::unique_ptr<NewcodePatch>>& newcode,
        const std::unordered_map<int, std::unique_ptr<AutogenDataInfo>>& autogen,
        const std::unordered_map<int, u32>& newcodeAddr,
        const void* shTable
    ) : patchInfo(&patches),
        rtreplPatches(&rtreplPatchesRef),
        newcodeDataForDest(&newcode),
        autogenDataInfoForDest(&autogen),
        newcodeAddrForDest(&newcodeAddr),
        sectionHeaderTable(shTable) {}
};

/**
 * Enumeration for patch types
 */
namespace PatchType {
    enum Value {
        Jump = 0,
        Call = 1,
        Hook = 2,
        Over = 3,
        SetJump = 4,
        SetCall = 5,
        SetHook = 6,
        RtRepl = 7,
        TJump = 8,
        TCall = 9,
        THook = 10,
        SetTJump = 11,
        SetTCall = 12,
        SetTHook = 13
    };
}

/**
 * Utility functions for working with patch types
 */
namespace PatchTypeUtils {
    constexpr const char* patchTypeNames[] = {
        "jump", "call", "hook", "over",
        "setjump", "setcall", "sethook", "rtrepl",
        "tjump", "tcall", "thook",
        "settjump", "settcall", "sethook"
    };
    
    constexpr std::size_t numPatchTypes = sizeof(patchTypeNames) / sizeof(char*);
    
    inline const char* getName(std::size_t patchType) {
        return (patchType < numPatchTypes) ? patchTypeNames[patchType] : "unknown";
    }
    
    inline bool isThumbPatch(std::size_t patchType) {
        return patchType >= PatchType::TJump && patchType <= PatchType::SetTHook;
    }
    
    inline bool isSetPatch(std::size_t patchType) {
        return patchType == PatchType::SetJump || patchType == PatchType::SetCall || 
               patchType == PatchType::SetHook || patchType == PatchType::SetTJump ||
               patchType == PatchType::SetTCall || patchType == PatchType::SetTHook;
    }
}

/**
 * Collection of common data structures used throughout the patch system
 */
struct PatchDataCollection
{
    std::vector<std::unique_ptr<GenericPatchInfo>> genericPatches;
    std::vector<std::unique_ptr<RtReplPatchInfo>> rtreplPatches;
    std::vector<std::unique_ptr<SectionInfo>> sections;
    std::vector<std::unique_ptr<OverwriteRegionInfo>> overwriteRegions;
    std::vector<std::string> externSymbols;
    std::unordered_map<int, std::unique_ptr<NewcodePatch>> newcodeData;
    std::unordered_map<int, std::unique_ptr<AutogenDataInfo>> autogenData;
    std::unordered_map<int, u32> newcodeAddresses;
    
    // Destinations with special handling
    std::vector<int> destWithNcpSet;
    core::CompilationUnitPtrCollection unitsWithNcpSet;
    
    // Analysis results
    std::unordered_map<std::string, std::unique_ptr<SymbolInfo>> symbolInfo;
    std::unordered_set<std::string> usedSymbols;
    std::unordered_set<std::string> usedSections;
};

} // namespace patch
