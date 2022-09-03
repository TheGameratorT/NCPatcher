#pragma once

#include <cstddef>
#include <vector>
#include <memory>
#include <mutex>
#include <filesystem>

#include "sourcefilejob.hpp"

class BuildLogger
{
public:
	BuildLogger();

	constexpr void setJobs(const std::vector<std::unique_ptr<SourceFileJob>>& jobs) { m_jobs = &jobs; }

	void start(const std::filesystem::path& targetRoot);
	void update();
	void finish();
	[[nodiscard]] constexpr bool getFailed() const { return m_failureFound; }

private:
	int m_cursorOffsetY;
	int m_currentFrame;
	bool m_failureFound;
	std::size_t m_filesToBuild;
	const std::vector<std::unique_ptr<SourceFileJob>>* m_jobs;
};
