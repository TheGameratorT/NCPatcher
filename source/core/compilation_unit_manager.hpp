#pragma once

#include <string>
#include <vector>
#include <memory>
#include <filesystem>

#include "compilation_unit.hpp"

namespace core {

class CompilationUnitManager
{
public:
    CompilationUnitManager();
    ~CompilationUnitManager();

	CompilationUnit* createCompilationUnit(CompilationUnitType type,
                                       const std::filesystem::path& sourcePath,
                                       const std::filesystem::path& objectPath);

	void destroyCompilationUnit(CompilationUnit* unit);

	CompilationUnitPtrCollection filterByRegion(const CompilationUnitPtrCollection& units, const BuildTarget::Region* region);
	CompilationUnit* findBySourcePath(const CompilationUnitPtrCollection& units, const std::filesystem::path& sourcePath);
    
    constexpr CompilationUnitOwnerCollection& getUnits() { return m_units; }
    constexpr const CompilationUnitOwnerCollection& getUnits() const { return m_units; }
    
    constexpr CompilationUnitPtrCollection& getUserUnits() { return m_userUnits; }
    constexpr const CompilationUnitPtrCollection& getUserUnits() const { return m_userUnits; }
    
    constexpr CompilationUnitPtrCollection& getLibraryUnits() { return m_libraryUnits; }
    constexpr const CompilationUnitPtrCollection& getLibraryUnits() const { return m_libraryUnits; }

private:
	CompilationUnitOwnerCollection m_units;
	CompilationUnitPtrCollection m_userUnits;
	CompilationUnitPtrCollection m_libraryUnits;
};

} // namespace core
