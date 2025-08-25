#pragma once

#include <vector>
#include <memory>
#include <string>
#include <filesystem>
#include <unordered_map>

#include "../utils/types.hpp"
#include "../core/compilation_unit_manager.hpp"
#include "../config/buildtarget.hpp"
#include "../formats/elf.hpp"
#include "../formats/archive.hpp"
#include "types.hpp"

// Use the centralized types from patch::types
using patch::SectionInfo;

/**
 * LibraryAnalyzer - Generates CompilationUnits for library object files
 * 
 * This class analyzes the linker flags to find library dependencies (-lc, -lgcc, -L etc.)
 * and creates CompilationUnits for object files within those libraries,
 * allowing them to be processed just like user object files.
 */
class LibraryAnalyzer
{
public:
    LibraryAnalyzer();
    ~LibraryAnalyzer();

    void initialize(
        const BuildTarget& target,
        const std::filesystem::path& buildDir,
        core::CompilationUnitManager& compilationUnitMgr
    );

    void analyzeLibraryDependencies();
    void generateLibraryUnits();

    // Getters for debugging
    const std::vector<std::filesystem::path>& getLibraryPaths() const { return m_libraryPaths; }
    const std::vector<std::string>& getLibraryNames() const { return m_libraryNames; }

private:
    void parseLinkerFlags(const std::string& ldFlags);
    void getToolchainLibraryPaths();
    void findLibraryFiles();
    void createUnitsFromLibrary(const std::filesystem::path& libraryPath);
    void createUnitsFromArchive(const std::filesystem::path& archivePath);
    void createUnitFromELF(const Elf32& elf, const std::filesystem::path& libraryPath, const std::string& objectName = "");

    const BuildTarget* m_target;
    const std::filesystem::path* m_buildDir;
    core::CompilationUnitManager* m_compilationUnitMgr;

    std::vector<std::string> m_librarySearchPaths;
    std::vector<std::string> m_libraryNames;
    std::vector<std::filesystem::path> m_libraryPaths;
};
