#pragma once

#include <vector>
#include <memory>
#include <unordered_map>

#include "../utils/types.hpp"
#include "../formats/elf.hpp"
#include "types.hpp"

// Use the centralized types from patch::types
using patch::GenericPatchInfo;
using patch::OverwriteRegionInfo;
using patch::NewcodePatch;
using patch::AutogenDataInfo;
using patch::PatchSourceType;

class ElfAnalyzer
{
public:
    ElfAnalyzer();
    ~ElfAnalyzer();

    void initialize(const std::filesystem::path& elfPath);
    void loadElfFile();
    void unloadElfFile();

    void gatherInfoFromElf(
        std::vector<std::unique_ptr<GenericPatchInfo>>& patchInfo,
        const std::vector<std::unique_ptr<OverwriteRegionInfo>>& overwriteRegions
    );

    const Elf32* getElf() const { return m_elf.get(); }

    // Data accessors
    const std::unordered_map<int, std::unique_ptr<NewcodePatch>>& getNewcodeDataForDest() const { return m_newcodeDataForDest; }
    const std::unordered_map<int, std::unique_ptr<AutogenDataInfo>>& getAutogenDataInfoForDest() const { return m_autogenDataInfoForDest; }

    // Move semantics for transfer
    std::unordered_map<int, std::unique_ptr<NewcodePatch>> takeNewcodeDataForDest() { return std::move(m_newcodeDataForDest); }
    std::unordered_map<int, std::unique_ptr<AutogenDataInfo>> takeAutogenDataInfoForDest() { return std::move(m_autogenDataInfoForDest); }

private:
    std::filesystem::path m_elfPath;
    std::unique_ptr<Elf32> m_elf;
    
    std::unordered_map<int, std::unique_ptr<NewcodePatch>> m_newcodeDataForDest;
    std::unordered_map<int, std::unique_ptr<AutogenDataInfo>> m_autogenDataInfoForDest;
};
