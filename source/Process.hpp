#pragma once

#include <string>

class Process
{
public:
	void setApp(const std::string& appName);
	void setArgs(const std::string& appArgs);
	bool start();
	int getExitCode();
	void setOutput(std::ostream* out);

private:
	std::string appName;
	std::string appArgs;

	int exitCode;

	std::ostream* output = nullptr;
};
