#include "process.hpp"

#include <algorithm>
#include <cctype>
#include <cwchar>
#include <cwctype>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "../utils/unicode.hpp"

#define BUFSIZE 4096

#ifdef _WIN32

#include <windows.h>

namespace {

bool sameEnvironmentName(const std::wstring& entry, const std::wstring& name)
{
	const std::size_t separator = entry.find(L'=', entry.starts_with(L'=') ? 1 : 0);
	if (separator != name.size())
		return false;
	for (std::size_t i = 0; i < name.size(); i++)
	{
		if (std::tolower(static_cast<unsigned char>(entry[i]))
			!= std::tolower(static_cast<unsigned char>(name[i])))
			return false;
	}
	return true;
}

// Wide, because the block is handed to CreateProcessW with
// CREATE_UNICODE_ENVIRONMENT. A narrow block would put every inherited variable
// through the ANSI code page on the way in and out, which is how a perfectly
// good PATH comes back with question marks in it.
std::vector<wchar_t> makeEnvironmentBlock(const Process::Environment& overrides)
{
	std::vector<std::wstring> entries;
	LPWCH inherited = GetEnvironmentStringsW();
	if (inherited == nullptr)
		throw std::runtime_error("GetEnvironmentStrings");
	for (const wchar_t* entry = inherited; *entry != L'\0'; entry += std::wcslen(entry) + 1)
		entries.emplace_back(entry);
	FreeEnvironmentStringsW(inherited);

	for (const auto& [name, value] : overrides)
	{
		const std::wstring wideName = ncp::toWide(name);
		const auto found = std::find_if(entries.begin(), entries.end(),
			[&](const std::wstring& entry) { return sameEnvironmentName(entry, wideName); });
		const std::wstring replacement = wideName + L'=' + ncp::toWide(value);
		if (found == entries.end())
			entries.push_back(replacement);
		else
			*found = replacement;
	}

	auto foldedLess = [](const std::wstring& left, const std::wstring& right) {
		return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end(),
			[](wchar_t a, wchar_t b) { return std::towlower(a) < std::towlower(b); });
	};
	std::sort(entries.begin(), entries.end(), foldedLess);

	std::vector<wchar_t> block;
	for (const std::wstring& entry : entries)
	{
		block.insert(block.end(), entry.begin(), entry.end());
		block.push_back(L'\0');
	}
	// One terminator ends the last entry; the second ends the block. An empty
	// environment still needs both.
	if (block.empty())
		block.push_back(L'\0');
	block.push_back(L'\0');
	return block;
}

} // namespace

int Process::start(const char* cmd, const std::filesystem::path& cwd,
	               const Environment& environment, std::ostream* out)
{
	std::vector<wchar_t> environmentBlock;
	if (!environment.empty())
		environmentBlock = makeEnvironmentBlock(environment);

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
	//
	// Wide, and CreateProcessW below. The narrow half of the pair encodes
	// through the machine's ANSI code page, which on an install whose user name
	// is not ASCII cannot spell the paths in a compiler command line at all,
	// and `cmd` is UTF-8 because every path in it came through ncp::pathToUtf8.
	const std::wstring wideCommand = ncp::toWide(cmd);
	std::vector<wchar_t> commandLine(wideCommand.begin(), wideCommand.end());
	commandLine.push_back(L'\0');

	PROCESS_INFORMATION piProcInfo;
	STARTUPINFOW siStartInfo;
	BOOL bSuccess;

	// Set up members of the PROCESS_INFORMATION structure.
	ZeroMemory(&piProcInfo, sizeof(PROCESS_INFORMATION));

	// Set up members of the STARTUPINFO structure.
	// This structure specifies the STDOUT handles for redirection.
	ZeroMemory(&siStartInfo, sizeof(STARTUPINFOW));
	siStartInfo.cb = sizeof(STARTUPINFOW);
	siStartInfo.hStdError = g_hChildStd_OUT_Wr;
	siStartInfo.hStdOutput = g_hChildStd_OUT_Wr;
	siStartInfo.dwFlags |= STARTF_USESTDHANDLES;

	// Create the child process. An empty cwd means "inherit ours".
	// native(), because a path is already UTF-16 here: this is the one string
	// in the call that needs no conversion at all.
	const std::wstring cwdStr = cwd.wstring();
	const wchar_t* lpCurrentDirectory = cwdStr.empty() ? NULL : cwdStr.c_str();

	LPVOID lpEnvironment = environmentBlock.empty() ? NULL : environmentBlock.data();
	const DWORD flags = environmentBlock.empty() ? 0 : CREATE_UNICODE_ENVIRONMENT;
	bSuccess = CreateProcessW(NULL, commandLine.data(), NULL, NULL, TRUE, flags,
		lpEnvironment, lpCurrentDirectory, &siStartInfo, &piProcInfo);
   
	// If an error occurs, exit the application. 
	if (!bSuccess)
	{
		CloseHandle(g_hChildStd_OUT_Rd);
		CloseHandle(g_hChildStd_OUT_Wr);
		throw std::runtime_error("CreateProcess");
	}

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
	wchar_t fullPath[MAX_PATH];
	return SearchPathW(nullptr, ncp::toWide(app).c_str(), L".exe", MAX_PATH, fullPath, nullptr) > 0;
}

#else

#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#define SHELL "/bin/sh"

int Process::start(const char* cmd, const std::filesystem::path& cwd,
	               const Environment& environment, std::ostream* out)
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

		// Only the child moves; the parent's cwd is left alone.
		if (!cwd.empty() && chdir(cwd.c_str()) != 0)
			_exit(EXIT_FAILURE);

		for (const auto& [name, value] : environment)
		{
			if (setenv(name.c_str(), value.c_str(), 1) != 0)
				_exit(EXIT_FAILURE);
		}

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

int Process::start(const char* cmd, const std::filesystem::path& cwd, std::ostream* out)
{
	return Process::start(cmd, cwd, Environment(), out);
}

int Process::start(const char* cmd, std::ostream* out)
{
	return Process::start(cmd, std::filesystem::path(), out);
}
