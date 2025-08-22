#pragma once

#include <vector>
#include <memory>
#include <unordered_map>

#include "../types.hpp"
#include "../elf.hpp"
#include "patch_info_analyzer.hpp"

// Forward declaration
struct OverwriteRegionInfo;

struct NewcodePatch
{
    const u8* binData;
    const u8* bssData;
    std::size_t binSize;
    std::size_t binAlign;
    std::size_t bssSize;
    std::size_t bssAlign;
};

struct AutogenDataInfo
{
    u32 address;
    u32 curAddress;
    std::vector<u8> data;
};

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

    static void forEachElfSection(
        const Elf32_Ehdr& eh, const Elf32_Shdr* sh_tbl, const char* str_tbl,
        const std::function<bool(std::size_t, const Elf32_Shdr&, std::string_view)>& cb
    );

    static void forEachElfSymbol(
        const Elf32& elf, const Elf32_Ehdr& eh, const Elf32_Shdr* sh_tbl,
        const std::function<bool(const Elf32_Sym&, std::string_view)>& cb
    );
};
