#include "buildlogger.hpp"

#include <iostream>

#include "../system/log.hpp"

static char s_progAnimFrames[] = { '-', '\\', '|', '/', '-', '\\', '|', '/' };

BuildLogger::BuildLogger() = default;

void BuildLogger::start(const std::filesystem::path& targetRoot)
{
	Log::out << OBUILD << "Starting..." << std::endl;

	Log::setMode(LogMode::Console);
#ifndef _WIN32
	Log::showCursor(false);
#endif

	m_currentFrame = 0;
	m_failureFound = false;
	m_filesToBuild = 0;

	for (const auto& unit : *m_units)
	{
		if (unit->needsRebuild())
			m_filesToBuild++;
	}

	std::size_t bufRemainingLines = Log::getRemainingLines();
	std::size_t bufLineShift = (bufRemainingLines < m_filesToBuild) ? (m_filesToBuild - bufRemainingLines) : 0;

	m_cursorOffsetY = Log::getXY().y - bufLineShift;

	for (const auto& unit : *m_units)
	{
		if (!unit->needsRebuild())
			continue;
		std::string filePath = unit->getSourcePath().string();
		Log::out << OBUILD << OSQRTBRKTS(ANSI_bWHITE, , "-") << ' ' << ANSI_bYELLOW << filePath << ANSI_RESET;
		Log::out << std::endl;
	}
}

void BuildLogger::update()
{
	for (const auto& unit : *m_units)
	{
		const auto& buildInfo = unit->getBuildInfo();
		
		if (!buildInfo.buildStarted || (buildInfo.buildComplete && buildInfo.logFinished))
			continue;
			
		const int writeX = 9;
		const int writeY = m_cursorOffsetY + int(buildInfo.jobId);
		
		if (buildInfo.buildComplete && !buildInfo.logFinished)
		{
			if (buildInfo.buildFailed)
			{
				Log::writeChar(writeX, writeY, 'E', Log::Red, true);
				m_failureFound = true;
			}
			else
			{
				Log::writeChar(writeX, writeY, 'S', Log::Green, true);
			}
			// Note: We need to modify this through the unit, not const reference
			const_cast<core::BuildInfo&>(buildInfo).logFinished = true;
		}
		else
		{
			Log::writeChar(writeX, writeY, s_progAnimFrames[m_currentFrame]);
		}
	}
	m_currentFrame++;
	if (m_currentFrame > 7)
		m_currentFrame = 0;
}

void BuildLogger::finish()
{
	update();
	Log::gotoXY(0, m_cursorOffsetY + int(m_filesToBuild));

	Log::setMode(LogMode::File);

	for (const auto& unit : *m_units)
	{
		if (!unit->needsRebuild())
			continue;
		const auto& buildInfo = unit->getBuildInfo();
		std::string filePath = unit->getSourcePath().string();
		Log::out << "[Build] [" << (buildInfo.buildFailed ? 'E' : 'S') << "] " << filePath;
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

	if (m_failureFound)
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

#ifndef _WIN32
	Log::showCursor(true);
#endif
}
