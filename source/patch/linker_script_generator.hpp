#pragma once

#include <vector>
#include <memory>
#include <string>
#include <unordered_map>
#include <filesystem>

#include "../types.hpp"
#include "../config/buildtarget.hpp"
#include "../build/sourcefilejob.hpp"
#include "patch_info_analyzer.hpp"
#include "overwrite_region_manager.hpp"

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
    std::vector<GenericPatchInfo*> sectionPatches;
};

struct LDSOverPatch
{
    GenericPatchInfo* info;
    LDSMemoryEntry* memory;
};

class LinkerScriptGenerator
{
public:
    LinkerScriptGenerator();
    ~LinkerScriptGenerator();

    void initialize(
        const BuildTarget& target,
        const std::filesystem::path& buildDir,
        const std::vector<std::unique_ptr<SourceFileJob>>& srcFileJobs,
        const std::unordered_map<int, u32>& newcodeAddrForDest
    );

    void createLinkerScript(
        const std::vector<std::unique_ptr<GenericPatchInfo>>& patchInfo,
        const std::vector<std::unique_ptr<RtReplPatchInfo>>& rtreplPatches,
        const std::vector<std::string>& externSymbols,
        const std::vector<int>& destWithNcpSet,
        const std::vector<const SourceFileJob*>& jobsWithNcpSet,
        const std::vector<std::unique_ptr<OverwriteRegionInfo>>& overwriteRegions
    );

    void linkElfFile();

private:
    const BuildTarget* m_target;
    const std::filesystem::path* m_buildDir;
    const std::vector<std::unique_ptr<SourceFileJob>>* m_srcFileJobs;
    const std::unordered_map<int, u32>* m_newcodeAddrForDest;
    
    std::filesystem::path m_ldscriptPath;
    std::filesystem::path m_elfPath;

    static std::string ldFlagsToGccFlags(std::string flags);
};
