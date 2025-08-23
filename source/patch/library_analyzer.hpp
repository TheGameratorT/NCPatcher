#pragma once

#include <vector>
#include <memory>
#include <string>
#include <filesystem>
#include <unordered_map>

#include "../types.hpp"
#include "../build/sourcefilejob.hpp"
#include "../config/buildtarget.hpp"
#include "../elf.hpp"
#include "../archive.hpp"
#include "types.hpp"

// Use the centralized types from patch::types
using patch::SectionInfo;

/**
 * LibraryAnalyzer - Generates SourceFileJobs for library object files
 * 
 * This class analyzes the linker flags to find library dependencies (-lc, -lgcc, -L etc.)
 * and creates SourceFileJobs for object files within those libraries,
 * allowing them to be processed just like user object files.
 */
class LibraryAnalyzer
{
public:
    LibraryAnalyzer();
    ~LibraryAnalyzer();

    void initialize(
        const BuildTarget& target,
        const std::vector<std::unique_ptr<SourceFileJob>>& srcFileJobs,
        const std::filesystem::path& buildDir
    );

    void analyzeLibraryDependencies();
    std::vector<std::unique_ptr<SourceFileJob>> generateLibraryJobs();

    // Getters for debugging
    const std::vector<std::filesystem::path>& getLibraryPaths() const { return m_libraryPaths; }
    const std::vector<std::string>& getLibraryNames() const { return m_libraryNames; }

private:
    void parseLinkerFlags(const std::string& ldFlags);
    void getToolchainLibraryPaths();
    void findLibraryFiles();
    void createJobsFromLibrary(const std::filesystem::path& libraryPath);
    void createJobsFromArchive(const std::filesystem::path& archivePath);
    void createJobFromELF(const Elf32& elf, const std::filesystem::path& libraryPath, const std::string& objectName = "");

    const BuildTarget* m_target;
    const std::vector<std::unique_ptr<SourceFileJob>>* m_srcFileJobs;
    const std::filesystem::path* m_buildDir;

    std::vector<std::string> m_librarySearchPaths;
    std::vector<std::string> m_libraryNames;
    std::vector<std::filesystem::path> m_libraryPaths;
    std::vector<std::unique_ptr<SourceFileJob>> m_libraryJobs;
};
