#pragma once

#include <vector>
#include <memory>
#include <string>
#include <functional>

#include "../types.hpp"
#include "../elf.hpp"
#include "../build/sourcefilejob.hpp"
#include "../config/buildtarget.hpp"

struct GenericPatchInfo
{
    u32 srcAddress; // the address of the symbol (only fetched after linkage)
    int srcAddressOv; // the overlay the address of the symbol (-1 arm, >= 0 overlay)
    u32 destAddress; // the address to be patched
    int destAddressOv; // the overlay of the address to be patched
    std::size_t patchType; // the patch type
    int sectionIdx; // the index of the section (-1 label, >= 0 section index)
    int sectionSize; // the size of the section (used for over patches)
    bool isNcpSet; // if the patch is an ncp_set type patch
    bool srcThumb; // if the function of the symbol is thumb
    bool destThumb; // if the function to be patched is thumb
    std::string symbol; // the symbol of the patch (used to generate linker script)
    SourceFileJob* job;
};

struct RtReplPatchInfo
{
    std::string symbol;
    SourceFileJob* job;
};

struct SectionInfo
{
    std::string name;
    std::size_t size;
    SourceFileJob* job;
    u32 alignment;
    u32 address = 0;
    const u8* data = nullptr;
    int destination = -1;
};

class PatchInfoAnalyzer
{
public:
    PatchInfoAnalyzer();
    ~PatchInfoAnalyzer();

    void initialize(
        const BuildTarget& target,
        const std::vector<std::unique_ptr<SourceFileJob>>& srcFileJobs,
        const std::filesystem::path& targetWorkDir
    );

    void gatherInfoFromObjects();

    // Getters
    const std::vector<std::unique_ptr<GenericPatchInfo>>& getPatchInfo() const { return m_patchInfo; }
    const std::vector<std::unique_ptr<RtReplPatchInfo>>& getRtreplPatches() const { return m_rtreplPatches; }
    const std::vector<std::string>& getExternSymbols() const { return m_externSymbols; }
    const std::vector<std::unique_ptr<SectionInfo>>& getOverwriteCandidateSections() const { return m_overwriteCandidateSections; }
    const std::vector<int>& getDestWithNcpSet() const { return m_destWithNcpSet; }
    const std::vector<const SourceFileJob*>& getJobsWithNcpSet() const { return m_jobsWithNcpSet; }

    // Move semantics for transfer to other components
    std::vector<std::unique_ptr<GenericPatchInfo>> takePatchInfo() { return std::move(m_patchInfo); }
    std::vector<std::unique_ptr<RtReplPatchInfo>> takeRtreplPatches() { return std::move(m_rtreplPatches); }
    std::vector<std::string> takeExternSymbols() { return std::move(m_externSymbols); }
    std::vector<std::unique_ptr<SectionInfo>> takeOverwriteCandidateSections() { return std::move(m_overwriteCandidateSections); }

private:
    const BuildTarget* m_target;
    const std::vector<std::unique_ptr<SourceFileJob>>* m_srcFileJobs;
    const std::filesystem::path* m_targetWorkDir;

    std::vector<std::unique_ptr<GenericPatchInfo>> m_patchInfo;
    std::vector<std::unique_ptr<RtReplPatchInfo>> m_rtreplPatches;
    std::vector<int> m_destWithNcpSet;
    std::vector<const SourceFileJob*> m_jobsWithNcpSet;
    std::vector<std::string> m_externSymbols;
    std::vector<std::unique_ptr<SectionInfo>> m_overwriteCandidateSections;

    static void forEachElfSection(
        const Elf32_Ehdr& eh, const Elf32_Shdr* sh_tbl, const char* str_tbl,
        const std::function<bool(std::size_t, const Elf32_Shdr&, std::string_view)>& cb
    );

    static void forEachElfSymbol(
        const Elf32& elf, const Elf32_Ehdr& eh, const Elf32_Shdr* sh_tbl,
        const std::function<bool(const Elf32_Sym&, std::string_view)>& cb
    );
};
