#include "buildlogger.hpp"

#include <iostream>

#include "../log.hpp"

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

	for (const std::unique_ptr<SourceFileJob>& job : *m_jobs)
	{
		if (job->rebuild)
			m_filesToBuild++;
	}

	std::size_t bufRemainingLines = Log::getRemainingLines();
	std::size_t bufLineShift = (bufRemainingLines < m_filesToBuild) ? (m_filesToBuild - bufRemainingLines) : 0;

	m_cursorOffsetY = Log::getXY().y - bufLineShift;

	for (const std::unique_ptr<SourceFileJob>& job : *m_jobs)
	{
		if (!job->rebuild)
			continue;
		std::string filePath = job->srcFilePath.string();
		Log::out << OBUILD << OSQRTBRKTS(ANSI_bWHITE, , "-") << ' ' << ANSI_bYELLOW << filePath << ANSI_RESET;
		Log::out << std::endl;
	}
}

void BuildLogger::update()
{
	for (const std::unique_ptr<SourceFileJob>& job : *m_jobs)
	{
		if (!job->buildStarted || (job->finished && job->logWasFinished))
			continue;
		const int writeX = 9;
		const int writeY = m_cursorOffsetY + int(job->jobID);
		if (job->finished && !job->logWasFinished)
		{
			if (job->failed)
			{
				Log::writeChar(writeX, writeY, 'E', Log::Red, true);
				m_failureFound = true;
			}
			else
			{
				Log::writeChar(writeX, writeY, 'S', Log::Green, true);
			}
			job->logWasFinished = true;
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

	for (const std::unique_ptr<SourceFileJob>& job : *m_jobs)
	{
		if (!job->rebuild)
			continue;
		std::string filePath = job->srcFilePath.string();
		Log::out << "[Build] [" << (job->failed ? 'E' : 'S') << "] " << filePath;
		Log::out << std::endl;
	}

	Log::setMode(LogMode::Both);

	auto printJobsOutput = [&](){
		for (const std::unique_ptr<SourceFileJob>& job : *m_jobs)
		{
			if (!job->output.empty())
			{
				Log::out << "\n-------- " << ANSI_bYELLOW << job->srcFilePath.string() << ANSI_RESET << " --------\n";
				Log::out << job->output << std::flush;
			}
		}
		Log::out << std::endl;
	};

	if (m_failureFound)
	{
		Log::out << "\nERRORS AND WARNINGS:\n";
		printJobsOutput();
	}
	else
	{
		bool foundWarnings = false;
		for (const std::unique_ptr<SourceFileJob>& job : *m_jobs)
		{
			if (!job->output.empty())
			{
				foundWarnings = true;
				break;
			}
		}
		if (foundWarnings)
		{
			Log::out << "\nWARNINGS:\n";
			printJobsOutput();
		}
	}

#ifndef _WIN32
	Log::showCursor(true);
#endif
}
