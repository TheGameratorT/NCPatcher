#pragma once

#include <vector>
#include <memory>
#include <string>
#include <functional>

#include "../types.hpp"
#include "../elf.hpp"
#include "../build/sourcefilejob.hpp"
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
        const std::filesystem::path& targetWorkDir
    );

    void gatherInfoFromObjects(const std::vector<std::unique_ptr<SourceFileJob>>& srcFileJobs);

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
    const std::filesystem::path* m_targetWorkDir;

    std::vector<std::unique_ptr<GenericPatchInfo>> m_patchInfo;
    std::vector<std::unique_ptr<RtReplPatchInfo>> m_rtreplPatches;
    std::vector<int> m_destWithNcpSet;
    std::vector<const SourceFileJob*> m_jobsWithNcpSet;
    std::vector<std::string> m_externSymbols;
    std::vector<std::unique_ptr<SectionInfo>> m_overwriteCandidateSections;

    // Helper methods
    bool loadElfFromArchive(Elf32& elf, const std::filesystem::path& archivePath, const std::string& memberName);
};
