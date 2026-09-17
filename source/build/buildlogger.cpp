#include "buildlogger.hpp"

#include <algorithm>
#include <cstdio>

#include "../system/log.hpp"
#include "progress_block.hpp"

BuildLogger::BuildLogger() = default;

void BuildLogger::start()
{
	Log::out << OBUILD << "Starting..." << std::endl;

	m_startTime = std::chrono::steady_clock::now();
	m_live = Log::terminalSupportsCursor();
	if (m_live)
		Log::showCursor(false);

	m_filesToBuild = 0;
	for (const auto& unit : *m_units)
	{
		if (unit->needsRebuild())
			m_filesToBuild++;
	}
}

void BuildLogger::update()
{
	if (!m_live)
		return;

	const auto now = std::chrono::steady_clock::now();

	ncp::build::ProgressState state;
	state.total = m_filesToBuild;

	for (const auto* unit : *m_units)
	{
		if (!unit->needsRebuild())
			continue;

		const auto& buildInfo = unit->getBuildInfo();

		if (buildInfo.buildComplete.load(std::memory_order_acquire))
		{
			state.completed++;
			continue;
		}

		if (!buildInfo.buildStarted.load(std::memory_order_acquire))
			continue;

		const std::string_view verb = (buildInfo.fileType == 2) ? "Assembling" : "Compiling";
		const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - buildInfo.startTime);
		state.running.push_back({verb, unit->getSourcePath().string(), elapsed});
	}

	std::sort(state.running.begin(), state.running.end(),
		[](const auto& a, const auto& b) { return a.elapsed > b.elapsed; });

	const Coords size = Log::terminalSize();
	const std::size_t maxActionLines = std::min<std::size_t>(8, std::size_t(std::max(1, size.y - 3)));

	m_block.render(ncp::build::renderProgressBlock(state, size.x, maxActionLines));
}

bool BuildLogger::getFailed() const
{
	for (const auto* unit : *m_units)
	{
		if (!unit->needsRebuild())
			continue;
		if (unit->getBuildInfo().buildFailed.load(std::memory_order_acquire))
			return true;
	}
	return false;
}

void BuildLogger::finish()
{
	if (m_live)
	{
		m_block.clear();
		Log::showCursor(true);
	}

	const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_startTime).count();

	char elapsedStr[32];
	std::snprintf(elapsedStr, sizeof(elapsedStr), "%.1f", elapsed);
	Log::out << OBUILD << "Compiled " << m_filesToBuild << " file"
		<< (m_filesToBuild == 1 ? "" : "s") << " in " << elapsedStr << "s" << std::endl;

	const bool failed = getFailed();

	Log::setMode(LogMode::File);

	for (const auto& unit : *m_units)
	{
		if (!unit->needsRebuild())
			continue;
		const auto& buildInfo = unit->getBuildInfo();
		std::string filePath = unit->getSourcePath().string();
		Log::out << "[Build] [" << (buildInfo.buildFailed.load(std::memory_order_acquire) ? 'E' : 'S') << "] " << filePath;
		Log::out << std::endl;
	}

	Log::setMode(LogMode::Both);

	auto printUnitsOutput = [&](){
		for (const auto& unit : *m_units)
		{
			const auto& buildInfo = unit->getBuildInfo();
			if (!buildInfo.buildOutput.empty())
			{
				Log::out << "\n-------- " << ANSI_bYELLOW << unit->getSourcePath().string() << ANSI_RESET << " --------\n";
				Log::out << buildInfo.buildOutput << std::flush;
			}
		}
		Log::out << std::endl;
	};

	if (failed)
	{
		Log::out << "\nERRORS AND WARNINGS:\n";
		printUnitsOutput();
	}
	else
	{
		bool foundWarnings = false;
		for (const auto& unit : *m_units)
		{
			const auto& buildInfo = unit->getBuildInfo();
			if (!buildInfo.buildOutput.empty())
			{
				foundWarnings = true;
				break;
			}
		}
		if (foundWarnings)
		{
			Log::out << "\nWARNINGS:\n";
			printUnitsOutput();
		}
	}
}
