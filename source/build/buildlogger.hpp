#pragma once

#include <cstddef>
#include <vector>
#include <memory>
#include <mutex>
#include <filesystem>

#include "../core/compilation_unit.hpp"

class BuildLogger
{
public:
	BuildLogger();

	constexpr void setUnits(const core::CompilationUnitPtrCollection& units) { m_units = &units; }

	void start(const std::filesystem::path& targetRoot);
	void update();
	void finish();
	[[nodiscard]] constexpr bool getFailed() const { return m_failureFound; }

private:
	int m_cursorOffsetY;
	int m_currentFrame;
	bool m_failureFound;
	std::size_t m_filesToBuild;
	const core::CompilationUnitPtrCollection* m_units;
};
