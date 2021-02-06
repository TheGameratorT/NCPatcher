#include <iostream>
#include <filesystem>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

#include "NCException.hpp"
#include "ConfigManager.hpp"
#include "NDSHeader.hpp"
#include "ARM.hpp"
#include "NCMaker.hpp"
#include "Common.hpp"

namespace fs = std::filesystem;

int main()
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

	fs::path asmSrcPath = "C:/Users/tiago/Desktop/LeBananaTest2/";
	fs::path romSrcPath = "C:/Users/tiago/Desktop/nds-rom-tools/nsmb_us/";

	try
	{
		ConfigManager cfgMgr(asmSrcPath);
		NDSHeader header(romSrcPath / "header.bin");

		ARM arm9(romSrcPath / "arm9.bin", header.arm9.entryAddress, header.arm9.ramAddress, 9);

		u32 codeAddr = arm9.read<u32>(cfgMgr.arenaLoAddr);
		std::cout << OINFO << "New code address: 0x" << codeAddr << std::endl;

		NCMaker maker(asmSrcPath, cfgMgr, codeAddr);
		maker.load();
		if (!maker.make())
			return 1;
	}
	catch (const NC::exception& e)
	{
		std::cout << e.what() << std::endl;
	}

	return 0;
}
