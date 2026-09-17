#pragma once

#include <chrono>
#include <cstddef>

#include "../core/compilation_unit.hpp"
#include "../system/live_block.hpp"

class BuildLogger
{
public:
	BuildLogger();

	constexpr void setUnits(const core::CompilationUnitPtrCollection& units) { m_units = &units; }

	void start();
	void update();
	void finish();
	[[nodiscard]] bool getFailed() const;

private:
	Log::LiveBlock m_block;
	std::chrono::steady_clock::time_point m_startTime;
	bool m_live = false;
	std::size_t m_filesToBuild = 0;
	const core::CompilationUnitPtrCollection* m_units = nullptr;
};
