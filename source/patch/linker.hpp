#pragma once

#include <vector>
#include <memory>
#include <string>
#include <unordered_map>
#include <filesystem>

#include "../utils/types.hpp"
#include "../formats/elf.hpp"
#include "../config/buildtarget.hpp"
#include "../app/context.hpp"
#include "../core/compilation_unit_manager.hpp"
#include "overwrite_region_manager.hpp"

namespace ncp::patch {

struct LDSMemoryEntry
{
    std::string name;
    u32 origin;
    int length;
};

struct LDSRegionEntry
{
    int dest;
    LDSMemoryEntry* memory;
    const BuildTarget::Region* region;
    std::size_t autogenDataSize;
    std::vector<PatchInfo*> sectionPatches;
};

struct LDSOverPatch
{
    PatchInfo* info;
    LDSMemoryEntry* memory;
};

class Linker
{
public:
    Linker();
    ~Linker();

    void initialize(
        const BuildTarget& target,
        const ncp::Context& ctx,
        core::CompilationUnitManager& compilationUnitMgr,
        const std::unordered_map<int, u32>& newcodeAddrForDest
    );

    void createLinkerScript(
        const std::vector<std::unique_ptr<PatchInfo>>& patchInfo,
        const std::vector<std::unique_ptr<PatchInfo>>& rtreplPatches,
        const std::vector<std::string>& externSymbols,
        const std::vector<std::unique_ptr<OverwriteRegionInfo>>& overwriteRegions
    );

    void linkElfFile();

    // Writes a throwaway linker script that brackets every overwrite candidate
    // section with __ncpm_<idx>_s / __ncpm_<idx>_e symbols, so linking it once
    // tells us the real post-GC, post-merge size of each candidate.
    void createMeasurementScript(
        const std::vector<std::unique_ptr<PatchInfo>>& patchInfo,
        const std::vector<std::unique_ptr<PatchInfo>>& rtreplPatches,
        const std::vector<std::string>& externSymbols,
        const std::vector<std::unique_ptr<OverwriteRegionInfo>>& overwriteRegions,
        const std::vector<std::unique_ptr<SectionInfo>>& candidateSections
    );

    void linkMeasurementElf();

    // Returns one entry per candidate (parallel to the vector passed to
    // createMeasurementScript), each the candidate's measured size, or 0 if it
    // did not survive linking.
    std::vector<u32> readMeasuredSizes(std::size_t candidateCount);

    void loadElfFile();
    void unloadElfFile();

    const Elf32* getElf() const { return m_elf.get(); }

private:
    enum class ScriptMode { Final, Measurement };

    const BuildTarget* m_target;
    const ncp::Context* m_ctx;
    const ncp::PathContext* m_paths;
    core::CompilationUnitManager* m_compilationUnitMgr;
    const std::unordered_map<int, u32>* m_newcodeAddrForDest;

    std::filesystem::path m_ldscriptPath;
    std::filesystem::path m_elfPath;
    std::filesystem::path m_measureLdscriptPath;
    std::filesystem::path m_measureElfPath;

    std::unique_ptr<Elf32> m_elf;

    void writeLinkerScript(
        ScriptMode mode,
        const std::vector<std::unique_ptr<PatchInfo>>& patchInfo,
        const std::vector<std::unique_ptr<PatchInfo>>& rtreplPatches,
        const std::vector<std::string>& externSymbols,
        const std::vector<std::unique_ptr<OverwriteRegionInfo>>& overwriteRegions,
        const std::vector<std::unique_ptr<SectionInfo>>* candidateSections
    );

    static std::string ldFlagsToGccFlags(std::string flags);
    //void parseLinkerOutput(const std::string& output);
};

} // namespace ncp::patch
