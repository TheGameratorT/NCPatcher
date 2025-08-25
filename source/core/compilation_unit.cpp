#include "compilation_unit.hpp"

#include <algorithm>
#include <stdexcept>

namespace core {

CompilationUnit::CompilationUnit(CompilationUnitType unitType, std::filesystem::path srcPath, std::filesystem::path objPath)
    : m_type(unitType)
    , m_sourcePath(std::move(srcPath))
    , m_objectPath(std::move(objPath))
{
    try {
        m_sourceWriteTime = std::filesystem::last_write_time(m_sourcePath);
    } catch (const std::filesystem::filesystem_error&) {
        // File may not exist yet (e.g., generated files), that's ok
        m_sourceWriteTime = std::filesystem::file_time_type{};
    }
}

} // namespace core
