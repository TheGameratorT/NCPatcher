#include "Process.hpp"

#include <iostream>
#include <sstream>
#include <algorithm>

#define BUFSIZE 4096

#ifdef _WIN32

#include <Windows.h>

static bool createChildProcess(const std::string& appName, const std::string& appArgs, PROCESS_INFORMATION& piProcInfo, HANDLE g_hChildStd_OUT_Wr);
static void readFromPipe(std::ostream* output, HANDLE g_hChildStd_OUT_Rd);

#else

#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#define SHELL "/bin/sh"

#endif

void Process::setApp(const std::string& appName)
{
	this->appName = appName;
}

void Process::setArgs(const std::string& appArgs)
{
	this->appArgs = appArgs;
}

void Process::setOutput(std::ostream* out)
{
	this->output = out;
}

int Process::getExitCode()
{
	return this->exitCode;
}

#ifdef _WIN32
bool Process::start()
{
	SECURITY_ATTRIBUTES sa;

	// Set the bInheritHandle flag so pipe handles are inherited.
	sa.nLength = sizeof(SECURITY_ATTRIBUTES);
	sa.bInheritHandle = TRUE;
	sa.lpSecurityDescriptor = NULL;

	HANDLE g_hChildStd_OUT_Rd = NULL;
	HANDLE g_hChildStd_OUT_Wr = NULL;

	// Create a pipe for the child process's STDOUT.
	CreatePipe(&g_hChildStd_OUT_Rd, &g_hChildStd_OUT_Wr, &sa, 0);

	// Ensure the read handle to the pipe for STDOUT is not inherited.
	SetHandleInformation(g_hChildStd_OUT_Rd, HANDLE_FLAG_INHERIT, 0);

	PROCESS_INFORMATION piProcInfo;
	// Set up members of the PROCESS_INFORMATION structure.
	ZeroMemory(&piProcInfo, sizeof(PROCESS_INFORMATION));

	bool created = createChildProcess(appName, appArgs, piProcInfo, g_hChildStd_OUT_Wr);
	readFromPipe(output, g_hChildStd_OUT_Rd);

	DWORD dwExitCode;
	GetExitCodeProcess(piProcInfo.hProcess, &dwExitCode);
	exitCode = dwExitCode;

	if(created)
		CloseHandle(piProcInfo.hProcess);

	return created;
}

static bool createChildProcess(const std::string& appName, const std::string& appArgs, PROCESS_INFORMATION& piProcInfo, HANDLE g_hChildStd_OUT_Wr)
{
	// Create the child process.
	std::string szCmdlineS = appName + " " + appArgs;
	TCHAR* szCmdline = szCmdlineS.data();

	STARTUPINFO siStartInfo;
	BOOL bSuccess = FALSE;

	// Set up members of the STARTUPINFO structure. 
	// This structure specifies the STDIN and STDOUT handles for redirection.

	ZeroMemory(&siStartInfo, sizeof(STARTUPINFO));
	siStartInfo.cb = sizeof(STARTUPINFO);
	siStartInfo.hStdError = g_hChildStd_OUT_Wr;
	siStartInfo.hStdOutput = g_hChildStd_OUT_Wr;
	siStartInfo.dwFlags |= STARTF_USESTDHANDLES;

	// Create the child process. 

	bSuccess = CreateProcess(NULL,
		szCmdline,     // command line 
		NULL,          // process security attributes 
		NULL,          // primary thread security attributes 
		TRUE,          // handles are inherited 
		0,             // creation flags 
		NULL,          // use parent's environment 
		NULL,          // use parent's current directory 
		&siStartInfo,  // STARTUPINFO pointer 
		&piProcInfo);  // receives PROCESS_INFORMATION 

	if (bSuccess)
	{
		// Close handles to the child process and its primary thread.
		// Some applications might keep these handles to monitor the status
		// of the child process, for example. 

		CloseHandle(piProcInfo.hThread);

		// Close handles to the stdin and stdout pipes no longer needed by the child process.
		// If they are not explicitly closed, there is no way to recognize that the child process has ended.

		CloseHandle(g_hChildStd_OUT_Wr);
	}

	return bSuccess;
}

static void readFromPipe(std::ostream* output, HANDLE g_hChildStd_OUT_Rd)
{
	DWORD dwRead;
	CHAR chBuf[BUFSIZE];
	BOOL bSuccess = FALSE;

	while (true)
	{
		bSuccess = ReadFile(g_hChildStd_OUT_Rd, chBuf, BUFSIZE, &dwRead, NULL);
		if (!bSuccess || dwRead == 0)
			break;

		std::string chBufS(chBuf, dwRead);

		size_t errPos = 0;
		while ((errPos = chBufS.find("\r\n", errPos)) != std::string::npos)
			chBufS.replace(errPos, 2, "\n");

		if (output)
			*output << chBufS;
	}

	CloseHandle(g_hChildStd_OUT_Rd);
}
#else
bool Process::start()
{
	std::string commandS = appName + " " + appArgs;
	const char* command = commandS.c_str();

	int pipefd[2];
	pipe(pipefd);

	int status;

	pid_t pid = fork();
	if (pid == 0)
	{
		close(pipefd[0]);    // close reading end in the child
		dup2(pipefd[1], 1);  // send stdout to the pipe
		dup2(pipefd[1], 2);  // send stderr to the pipe
		close(pipefd[1]);    // this descriptor is no longer needed

		// This is the child process.  Execute the shell command.
		execl(SHELL, SHELL, "-c", command, NULL);
	}
	else if (pid < 0)
	{
		// The fork failed. Report failure.
		status = false;
	}
	else
	{
		char buffer[BUFSIZE];
		close(pipefd[1]);  // close the write end of the pipe in the parent

		int len;
		while (true)
		{
			len = read(pipefd[0], buffer, sizeof(buffer));
			if(len == 0)
				break;

			if (output)
				output->write(buffer, len);
		}

		// This is the parent process.  Wait for the child to complete.
		status = (waitpid (pid, &exitCode, 0) == pid);
	}
	
	return status;
}
#endif
