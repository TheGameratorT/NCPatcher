#include "buildlogger.hpp"

#include <iostream>

#include "../log.hpp"

enum class BuildState
{
	Waiting,
	Started,
	Building,
	Built,
	Finished
};

static char s_progAnimFrames[] = { '-', '\\', '|', '/', '-', '\\', '|', '/' };

BuildLogger::BuildLogger() = default;

void BuildLogger::start(const std::filesystem::path& targetRoot)
{
	Log::out << OBUILD << "Starting..." << std::endl;

	Log::setMode(LogMode::Console);
	Log::showCursor(false);

	m_cursorOffsetY = Log::getXY().y;
	m_currentFrame = 0;
	m_failureFound = false;
	m_filesToBuild = 0;

	for (const std::unique_ptr<SourceFileJob>& job : *m_jobs)
	{
		if (!job->rebuild)
			continue;
		m_filesToBuild++;
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
		Log::gotoXY(9, m_cursorOffsetY + int(job->jobID));
		if (job->finished && !job->logWasFinished)
		{
			if (job->failed)
			{
				Log::out << ANSI_bRED "E" ANSI_RESET;
				m_failureFound = true;
			}
			else
			{
				Log::out << ANSI_bGREEN "S" ANSI_RESET;
			}
			job->logWasFinished = true;
		}
		else
		{
			Log::out.put(s_progAnimFrames[m_currentFrame]);
		}
		Log::out.flush();
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

	Log::showCursor(true);
}
