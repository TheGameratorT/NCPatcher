#include <iostream>
#include <cstring>
#include <fstream>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "common.hpp"
#include "global.hpp"
#include "except.hpp"
#include "arm.hpp"
#include "buildcfg.hpp"
#include "ndsheader.hpp"
#include "buildtarget.hpp"
#include "codemaker.hpp"
#include "codepatcher.hpp"
#include "oansistream.hpp"

namespace fs = std::filesystem;

static void WorkMain();
static bool GetExePath(fs::path& out);

int main(int argc, char* argv[])
{
#ifdef _WIN32
	// Set output mode to handle virtual terminal sequences
	HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
	if (hOut != INVALID_HANDLE_VALUE)
	{
		DWORD dwMode = 0;
		if (GetConsoleMode(hOut, &dwMode))
		{
			dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
			SetConsoleMode(hOut, dwMode);
		}
	}
#endif

	std::ios_base::sync_with_stdio(false); // Enable buffered printing

	ansi::cout << ANSI_bWHITE "[" ANSI_bGREEN "Nitro Code Patcher" ANSI_bWHITE "]" ANSI_RESET "\n" << std::flush;

	if (!GetExePath(ncp::exe_dir))
	{
		std::cout << "yo wat?? failed to get exe path, maybe too big???. flushed" << std::endl;
		return 1;
	}
	ncp::exe_dir = ncp::exe_dir.parent_path();

	if (argc == 2)
	{
		ncp::asm_path = fs::current_path();
		ncp::rom_path = argv[1];

		try
		{
			WorkMain();
		}
		catch (ncp::exception& e)
		{
			ansi::cout << OERROR << ncp::getErrorMsg() << "\n" << OREASON << e.what() << std::endl;
			return 1;
		}
	}
	else
	{
		ansi::cout << OINFO << "Usage is \"ncpatcher <rom_path>\"\n" << std::flush;
	}

	return 0;
}

static void WorkMain()
{
	ansi::cout << OINFO << "ASM path: " << OSTR(ncp::asm_path.string()) << std::endl;
	ansi::cout << OINFO << "ROM path: " << OSTR(ncp::rom_path.string()) << std::endl;

	if (!fs::exists(ncp::asm_path))
	{
		ncp::setErrorMsg("Could not find \"rom_path\" directory");
		throw ncp::dir_error(ncp::asm_path, ncp::dir_error::find);
	}

	if (!fs::exists(ncp::rom_path))
	{
		ncp::setErrorMsg("Could not find \"rom_path\" directory");
		throw ncp::dir_error(ncp::rom_path, ncp::dir_error::find);
	}

	//fs::current_path(ncp::asm_path);

	BuildCfg& buildCfg = ncp::build_cfg;
	buildCfg.load("build.json");

	NDSHeader& header = ncp::header_bin;
	header.load(ncp::rom_path / "header.bin");

	if (buildCfg.arm7 != "")
	{
		fs::current_path(buildCfg.arm7);
		BuildTarget target;
		ncp::setErrorMsg("Could not compile the ARM7 sources.");
		CodeMaker maker(target);
		CodePatcher patcher(target, maker.getBuiltRegions(), 7);
	}

	if (buildCfg.arm9 != "")
	{
		fs::current_path(buildCfg.arm9);
		BuildTarget target;
		ncp::setErrorMsg("Could not compile the ARM9 sources.");
		CodeMaker maker(target);
		CodePatcher patcher(target, maker.getBuiltRegions(), 9);
	}
}

bool GetExePath(fs::path& out)
{
#ifdef _WIN32
	wchar_t buffer[MAX_PATH];
	if (!GetModuleFileNameW(NULL, buffer, MAX_PATH))
		return false;
#else
	char buffer[PATH_MAX];
	if (readlink("/proc/self/exe", buffer, PATH_MAX) == -1)
		return false;
#endif
	out = buffer;
	return true;
}
