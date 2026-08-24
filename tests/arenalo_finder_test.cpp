#include "../source/ndsbin/armbin.hpp"
#include "../source/patch/arenalo_finder.hpp"
#include "../source/utils/util.hpp"

#include <cstring>
#include <iostream>
#include <vector>

namespace {

constexpr u32 ArmRam = 0x02380000;
constexpr u32 Wram = 0x037F8000;

ArmBin makeArm7(u32 arenaStart)
{
	constexpr std::size_t AutoloadOffset = 0x100;
	constexpr std::size_t AutoloadSize = 0x40;
	constexpr std::size_t AutoloadListOffset = AutoloadOffset + AutoloadSize;
	constexpr std::size_t ModuleParamsOffset = 0x40;

	std::vector<u8> data(AutoloadListOffset + 12);
	Util::write<u32>(&data[0x1C], ArmRam + ModuleParamsOffset);

	ArmBin::ModuleParams params{};
	params.autoloadListStart = ArmRam + AutoloadListOffset;
	params.autoloadListEnd = params.autoloadListStart + 12;
	params.autoloadStart = ArmRam + AutoloadOffset;
	params.staticBssStart = params.autoloadStart;
	params.staticBssEnd = params.autoloadStart;
	std::memcpy(&data[ModuleParamsOffset], &params, sizeof(params));

	const u32 entry[] = { Wram, u32(AutoloadSize), 0 };
	std::memcpy(&data[AutoloadListOffset], entry, sizeof(entry));

	// mov r0, #0x03800000; ldr r1, literal; cmp; movhi; bx lr
	const u32 code[] = {
		0xE3A0050E, 0xE59F1008, 0xE351050E, 0x81A00001, 0xE12FFF1E, arenaStart
	};
	std::memcpy(&data[AutoloadOffset], code, sizeof(code));

	ArmBin arm;
	arm.load(std::move(data), ArmRam, ArmRam, ArmRam + 0x20, false);
	return arm;
}

bool check(u32 arenaStart, u32 expectedDestination)
{
	ArmBin arm = makeArm7(arenaStart);
	int arenaLo = 0;
	u32 destination = 0;
	ncp::patch::ArenaLoFinder::findArenaLo(&arm, arenaLo, destination);

	const bool passed = arenaLo == int(Wram + 20) && destination == expectedDestination;
	if (!passed)
		std::cout << "FAIL: got arenaLo 0x" << std::hex << arenaLo
			<< " and destination 0x" << destination << '\n';
	return passed;
}

} // namespace

int main()
{
	const bool aboveSplit = check(0x03801234, 0x03801234);
	const bool belowSplit = check(0x037FF000, 0x03800000);
	if (aboveSplit && belowSplit)
		std::cout << "arenalo_finder_test: all checks passed\n";
	return aboveSplit && belowSplit ? 0 : 1;
}
