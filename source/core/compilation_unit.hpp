#pragma once

#include <string>
#include <filesystem>
#include <cstddef>
#include <chrono>
#include <memory>
#include <vector>
#include <algorithm>

#include "../config/buildtarget.hpp"

// Forward declaration
class Elf32;

namespace core {

/**
 * Enumeration for different types of compilation units
 */
enum class CompilationUnitType : std::size_t
{
    UserSourceFile = 0,
    LibraryFile = 1
};

/**
 * Build-specific information for compilation units
 */
struct BuildInfo
{
    std::filesystem::path dependencyPath;
    std::filesystem::path assemblyPath;
    std::filesystem::file_time_type objectWriteTime;
    
    // Source file type information
    std::size_t fileType = 0; // 0=C, 1=CPP, 2=ASM
    
    // Build execution state
    std::size_t jobId = 0;
    bool buildStarted = false;
    bool logFinished = false;
    bool buildComplete = false;
    bool buildFailed = false;
    std::string buildOutput;
};

/**
 * Complete compilation unit containing both core information and build information
 */
class CompilationUnit
{
public:
    CompilationUnit(CompilationUnitType unitType, std::filesystem::path srcPath, std::filesystem::path objPath);
    
    // Core properties
    CompilationUnitType getType() const { return m_type; }
    const std::filesystem::path& getSourcePath() const { return m_sourcePath; }
    const std::filesystem::path& getObjectPath() const { return m_objectPath; }
    
    // Build target association
    const BuildTarget::Region* getTargetRegion() const { return m_targetRegion; }
    void setTargetRegion(const BuildTarget::Region* region) { m_targetRegion = region; }
    
    // Metadata for tracking
    std::filesystem::file_time_type getSourceWriteTime() const { return m_sourceWriteTime; }
    void setSourceWriteTime(std::filesystem::file_time_type time) { m_sourceWriteTime = time; }
    
    bool needsRebuild() const { return m_needsRebuild; }
    void setNeedsRebuild(bool needs) { m_needsRebuild = needs; }
    
    // Build info access
    BuildInfo& getBuildInfo() { return m_buildInfo; }
    const BuildInfo& getBuildInfo() const { return m_buildInfo; }

    // ELF access for performance optimization
    Elf32* getElf() const { return m_elf; }
    void setElf(Elf32* elf) { m_elf = elf; }

private:
    CompilationUnitType m_type;
    std::filesystem::path m_sourcePath;
    std::filesystem::path m_objectPath;
    
    // Build target association
    const BuildTarget::Region* m_targetRegion = nullptr;
    
    // Metadata for tracking
    std::filesystem::file_time_type m_sourceWriteTime;
    bool m_needsRebuild = false;
    
    // ELF reference for performance optimization
    Elf32* m_elf = nullptr;
    
    // Build information integrated directly
    BuildInfo m_buildInfo;
};

// Type aliases for convenience
using CompilationUnitOwnerCollection = std::vector<std::unique_ptr<CompilationUnit>>;
using CompilationUnitPtrCollection = std::vector<CompilationUnit*>;

} // namespace core
