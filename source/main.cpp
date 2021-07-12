#include <iostream>
#include <cstring>
#include <fstream>

#include "common.hpp"
#include "global.hpp"
#include "except.hpp"
#include "oansistream.hpp"
#include "buildcfg.hpp"
#include "ndsheader.hpp"
#include "buildtarget.hpp"
#include "arm.hpp"

namespace fs = std::filesystem;

static void WorkMain();
static void MakeTarget(BuildTarget& target, ARM& arm);

int main(int argc, char* argv[])
{
	std::cout.sync_with_stdio(false); // Enable buffered printing

	ansi::cout << ANSI_bWHITE "[" ANSI_bGREEN "Nitro Code Patcher" ANSI_bWHITE "]" ANSI_RESET "\n" << std::flush;

	if (argc == 3)
	{
		ncp::asm_path = argv[1];
		ncp::rom_path = argv[2];

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
		ansi::cout << OINFO << "Usage is \"ncpatcher <asm_path> <rom_path>\"\n" << std::flush;
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

	fs::current_path(ncp::asm_path);

	BuildCfg buildCfg;
	buildCfg.load("build.json");

	NDSHeader& header = ncp::header_bin;
	header.load(ncp::rom_path / "header.bin");

	if (buildCfg.arm7 != "")
	{
		ncp::setErrorMsg("Could not load ARM7 workspace.");
		BuildTarget target(buildCfg.arm7);
		ARM arm7(ncp::rom_path / "arm7.bin", header.arm7.entryAddress, header.arm7.ramAddress, header.arm7AutoLoadListHookOffset, 7);
		MakeTarget(target, arm7);
	}

	if (buildCfg.arm9 != "")
	{
		ncp::setErrorMsg("Could not load ARM9 workspace.");
		BuildTarget target(buildCfg.arm9);
		ARM arm9(ncp::rom_path / "arm9.bin", header.arm9.entryAddress, header.arm9.ramAddress, header.arm9AutoLoadListHookOffset, 9);
		MakeTarget(target, arm9);
	}
}

static void MakeTarget(BuildTarget& target, ARM& arm)
{
	u32 newCode = arm.read<u32>(target.arenaLo);
	ansi::cout << OINFO << "New code start: " << "0x" << std::hex << std::uppercase << newCode << std::endl;
}
