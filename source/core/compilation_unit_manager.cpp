#include "compilation_unit_manager.hpp"

#include <algorithm>

namespace core {

CompilationUnitManager::CompilationUnitManager()
{
}

CompilationUnitManager::~CompilationUnitManager()
{
}

CompilationUnit* CompilationUnitManager::createCompilationUnit(CompilationUnitType type,
                                       const std::filesystem::path& sourcePath,
                                       const std::filesystem::path& objectPath)
{
	auto unit = std::make_unique<CompilationUnit>(type, sourcePath, objectPath);

	CompilationUnit* unitPtr = unit.get();

	m_units.push_back(std::move(unit));

	if (type == CompilationUnitType::UserSourceFile)
	{
		m_userUnits.push_back(unitPtr);
	}
	else if (type == CompilationUnitType::LibraryFile)
	{
		m_libraryUnits.push_back(unitPtr);
	}

    return unitPtr;
}

void CompilationUnitManager::destroyCompilationUnit(CompilationUnit* unit)
{
    // Remove from m_units and delete
    auto it = std::find_if(m_units.begin(), m_units.end(),
        [unit](const std::unique_ptr<CompilationUnit>& ptr) { return ptr.get() == unit; });
    if (it != m_units.end()) {
        m_units.erase(it);
    }

	core::CompilationUnitType unitType = unit->getType();

	if (unitType == CompilationUnitType::UserSourceFile)
	{
		// Remove from m_userUnits
		m_userUnits.erase(
			std::remove(m_userUnits.begin(), m_userUnits.end(), unit),
			m_userUnits.end()
		);
	}
	else if (unitType == CompilationUnitType::LibraryFile)
	{
		// Remove from m_libraryUnits
		m_libraryUnits.erase(
			std::remove(m_libraryUnits.begin(), m_libraryUnits.end(), unit),
			m_libraryUnits.end()
		);
	}
}

CompilationUnitPtrCollection CompilationUnitManager::filterByRegion(const CompilationUnitPtrCollection& units, const BuildTarget::Region* region)
{
    CompilationUnitPtrCollection result;
    std::copy_if(units.begin(), units.end(), std::back_inserter(result),
                 [region](const CompilationUnit* unit) {
                     return unit->getTargetRegion() == region;
                 });
    return result;
}

CompilationUnit* CompilationUnitManager::findBySourcePath(const CompilationUnitPtrCollection& units, const std::filesystem::path& sourcePath)
{
    auto it = std::find_if(units.begin(), units.end(),
                          [&sourcePath](const CompilationUnit* unit) {
                              return unit->getSourcePath() == sourcePath;
                          });
    return it != units.end() ? *it : nullptr;
}

} // namespace core
