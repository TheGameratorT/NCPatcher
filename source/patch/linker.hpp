#pragma once

#include <vector>
#include <memory>
#include <string>
#include <unordered_map>
#include <filesystem>

#include "../utils/types.hpp"
#include "../formats/elf.hpp"
#include "../config/buildtarget.hpp"
#include "../system/path_context.hpp"
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
        const ncp::PathContext& paths,
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

    void loadElfFile();
    void unloadElfFile();

    const Elf32* getElf() const { return m_elf.get(); }

private:
    const BuildTarget* m_target;
    const ncp::PathContext* m_paths;
    core::CompilationUnitManager* m_compilationUnitMgr;
    const std::unordered_map<int, u32>* m_newcodeAddrForDest;
    
    std::filesystem::path m_ldscriptPath;
    std::filesystem::path m_elfPath;

    std::unique_ptr<Elf32> m_elf;

    static std::string ldFlagsToGccFlags(std::string flags);
    //void parseLinkerOutput(const std::string& output);
};

} // namespace ncp::patch
