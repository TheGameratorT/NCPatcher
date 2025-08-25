#include "process.hpp"

#include <string>
#include <stdexcept>
#include <thread>
#include <vector>

#define BUFSIZE 4096

#ifdef _WIN32

#include <windows.h>
#include <tchar.h>

int Process::start(const char* cmd, std::ostream* out)
{
	HANDLE g_hChildStd_OUT_Rd = NULL;
	HANDLE g_hChildStd_OUT_Wr = NULL;

	SECURITY_ATTRIBUTES saAttr;

	// Set the bInheritHandle flag so pipe handles are inherited.
	saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
	saAttr.bInheritHandle = TRUE;
	saAttr.lpSecurityDescriptor = NULL;

	// Create a pipe for the child process's STDOUT.
	if (!CreatePipe(&g_hChildStd_OUT_Rd, &g_hChildStd_OUT_Wr, &saAttr, 0))
		throw std::runtime_error("StdoutRd CreatePipe");

	// Ensure the read handle to the pipe for STDOUT is not inherited.
	if (!SetHandleInformation(g_hChildStd_OUT_Rd, HANDLE_FLAG_INHERIT, 0))
	{
		CloseHandle(g_hChildStd_OUT_Rd);
		CloseHandle(g_hChildStd_OUT_Wr);
		throw std::runtime_error("Stdout SetHandleInformation");
	}

	// Create a child process that uses the previously created pipes for STDOUT.
	TCHAR* szCmdline = (TCHAR*)cmd;
	PROCESS_INFORMATION piProcInfo;
	STARTUPINFO siStartInfo;
	BOOL bSuccess;

	// Set up members of the PROCESS_INFORMATION structure.
	ZeroMemory(&piProcInfo, sizeof(PROCESS_INFORMATION));

	// Set up members of the STARTUPINFO structure.
	// This structure specifies the STDOUT handles for redirection.
	ZeroMemory(&siStartInfo, sizeof(STARTUPINFO));
	siStartInfo.cb = sizeof(STARTUPINFO);
	siStartInfo.hStdError = g_hChildStd_OUT_Wr;
	siStartInfo.hStdOutput = g_hChildStd_OUT_Wr;
	siStartInfo.dwFlags |= STARTF_USESTDHANDLES;

	// Create the child process.
	bSuccess = CreateProcess(NULL, szCmdline, NULL, NULL, TRUE, 0, NULL, NULL, &siStartInfo, &piProcInfo);
   
	// If an error occurs, exit the application. 
	if (!bSuccess)
		throw std::runtime_error("CreateProcess");

	// Close handle to the child process primary thread.
	CloseHandle(piProcInfo.hThread);

	// Close handles to the stdin and stdout pipes no longer needed by the child process.
	// If they are not explicitly closed, there is no way to recognize that the child process has ended.
	CloseHandle(g_hChildStd_OUT_Wr);
 
	// Read output from the child process's pipe for STDOUT
	// and write to the parent process's pipe for STDOUT.
	// Stop when there is no more data.
	DWORD dwRead;
	CHAR chBuf[BUFSIZE];
	while (true)
	{
		bSuccess = ReadFile(g_hChildStd_OUT_Rd, chBuf, BUFSIZE, &dwRead, NULL);
		if (!bSuccess || dwRead == 0)
			break;

		if (out != nullptr)
		{
			std::string chBufS(chBuf, dwRead);

			size_t errPos = 0;
			while ((errPos = chBufS.find("\r\n", errPos)) != std::string::npos)
				chBufS.replace(errPos, 2, "\n");

			*out << chBufS;
		}
	}

	// Close the read handle.
	CloseHandle(g_hChildStd_OUT_Rd);

	// Get the return code.
	DWORD dwExitCode;
	GetExitCodeProcess(piProcInfo.hProcess, &dwExitCode);

	// Close handle to the child process.
	CloseHandle(piProcInfo.hProcess);

	return int(dwExitCode);
}

bool Process::exists(const char* app)
{
	char fullPath[MAX_PATH];
	return SearchPathA(nullptr, app, ".exe", MAX_PATH, fullPath, nullptr) > 0;
}

#else

#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#define SHELL "/bin/sh"

int Process::start(const char* cmd, std::ostream* out)
{
	int pipefd[2];
	if (pipe(pipefd) < 0)
		throw std::runtime_error("Process pipe(pipefd) failed");

	int status;

	pid_t pid = fork();
	if (pid < 0)
		throw std::runtime_error("Process fork() failed");

	if (pid == 0) // Child
	{
		close(pipefd[0]); // Close the unused read end

		dup2(pipefd[1], STDOUT_FILENO); // Send stdout to the pipe
		dup2(pipefd[1], STDERR_FILENO); // Send stderr to the pipe
		close(pipefd[1]);               // This descriptor is no longer needed

		execl(SHELL, SHELL, "-c", cmd, NULL); // Execute the shell command
		_exit(EXIT_FAILURE);
	}
	else // Parent
	{
		close(pipefd[1]); // Close the unused write end

		// Use a separate thread to read from the pipe to prevent deadlock
		std::vector<char> output_buffer;
		std::thread reader_thread([&]() {
			char buffer[BUFSIZE];
			ssize_t len;
			while ((len = read(pipefd[0], buffer, sizeof(buffer))) > 0)
			{
				if (out)
					out->write(buffer, len);
				else
				{
					// Even if no output stream is provided, we need to consume the data
					// to prevent the child process from blocking on writes
					output_buffer.insert(output_buffer.end(), buffer, buffer + len);
				}
			}
			close(pipefd[0]); // Close the read end
		});
		
		// Wait for the child to complete
		if (waitpid(pid, &status, 0) != pid)
			status = -1;
		else if (WIFEXITED(status))
			status = WEXITSTATUS(status);
		else
			status = -1;
		
		// Wait for the reader thread to finish
		reader_thread.join();
	}

	return status;
}

bool Process::exists(const char* app)
{
	std::string cmd = std::string("which ") + app;
	return Process::start(cmd.c_str()) == 0;
}

#endif
