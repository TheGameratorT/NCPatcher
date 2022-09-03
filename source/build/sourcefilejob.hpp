#pragma once

#include <cstddef>
#include <string>
#include <filesystem>

#include "../config/buildtarget.hpp"

class SourceFileJob
{
public:
	std::filesystem::path srcFilePath;
	std::filesystem::path objFilePath;
	std::filesystem::path depFilePath;

	std::filesystem::file_time_type objFileWriteTime;
	std::size_t fileType;

	const BuildTarget::Region* region;

	bool rebuild = false;

	std::size_t jobID = 0;
	bool buildStarted = false;
	bool logWasFinished = false;
	bool finished = false;
	bool failed = false;
	std::string output;
};
