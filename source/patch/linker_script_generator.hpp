#pragma once

#include <vector>
#include <memory>
#include <string>
#include <unordered_map>
#include <filesystem>

#include "../utils/types.hpp"
#include "../config/buildtarget.hpp"
#include "../core/compilation_unit_manager.hpp"
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
        core::CompilationUnitManager& compilationUnitMgr,
        const std::unordered_map<int, u32>& newcodeAddrForDest
    );

    void createLinkerScript(
        const std::vector<std::unique_ptr<GenericPatchInfo>>& patchInfo,
        const std::vector<std::unique_ptr<RtReplPatchInfo>>& rtreplPatches,
        const std::vector<std::string>& externSymbols,
        const std::vector<int>& destWithNcpSet,
    	const core::CompilationUnitPtrCollection& unitsWithNcpSet,
        const std::vector<std::unique_ptr<OverwriteRegionInfo>>& overwriteRegions
    );

    void linkElfFile();

    // Path accessors
    const std::filesystem::path& getStripElfPath() const { return m_elfStripPath; }

private:
    const BuildTarget* m_target;
    const std::filesystem::path* m_buildDir;
    core::CompilationUnitManager* m_compilationUnitMgr;
    const std::unordered_map<int, u32>* m_newcodeAddrForDest;
    
    std::filesystem::path m_ldscriptStripPath;
    std::filesystem::path m_ldscriptPath;
    std::filesystem::path m_elfStripPath;
    std::filesystem::path m_elfPath;

    static std::string ldFlagsToGccFlags(std::string flags);
    //void parseLinkerOutput(const std::string& output);
};
