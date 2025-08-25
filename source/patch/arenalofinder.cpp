#include "arenalofinder.hpp"

#include <iostream>
#include <algorithm>

#include "../ndsbin/armbin.hpp"
#include "../system/log.hpp"
#include "../system/except.hpp"
#include "../utils/util.hpp"

namespace ArenaLoFinder {

struct PatternMatches
{
    const std::vector<u8> switchCase;
    const std::vector<u8> ldr;
    const std::vector<std::vector<u8>> ldmia;
    const std::vector<std::vector<u8>> reference; // For detecting OS_GetInitArenaHi
};

static struct PatternMatches patternMatchesArm = {
    .switchCase = { 0x06, 0x00, 0x50, 0xe3, 0x00, 0xf1, 0x8f, 0x90 }, // cmp r0, #0x6; addls pc, pc, r0, lsl #0x2
    .ldr = { 0x00, 0x9f, 0xe5 }, // ldr r0, [address]
    .ldmia = {
        { 0x00, 0x40, 0xbd, 0xe8 }, // ldmia sp!, {lr}
        { 0x08, 0x80, 0xbd, 0xe8 }, // ldmia sp!, {lr}
        { 0x1e, 0xff, 0x2f, 0xe1 }, // bx lr
    },
    .reference = {
        { 0x27, 0x06, 0xa0 }, // 0x2700000
        { 0x3c, 0x00, 0xa0 }, // 0x23c0000
        { 0x20, 0x00, 0xa0 }, // 0x2000000
    },
};

static struct PatternMatches patternMatchesThumb = {
    .switchCase = { 0x08, 0xb5, 0x06, 0x28 }, // push {r3, lr}; cmp r0, #0x6
    .ldr = { 0x48, 0x08, 0xbd }, // ldr r0, [address]; pop {r3, pc}
    .reference = {
        { 0x27, 0x20, 0x00, 0x05 }, // 0x2700000
        { 0x02, 0x20, 0x00, 0x06 }, // 0x2000000
    },
};

static const std::vector<int> findPattern(const std::vector<u8>& data, const std::vector<u8>& pattern, u32 start, s32 end)
{
    std::vector<int> matches;
    std::size_t pattern_length = pattern.size();
    for (std::size_t i = start; i < end - pattern_length + 1; i++)
    {
        if (std::equal(pattern.begin(), pattern.end(), data.begin() + i))
        {
            matches.push_back(i);
        }
    }
    return matches;
}

// Searches in the arm9 binary for OS_GetInitArenaLo, and finds the address of arenaLo
static bool processMatches(ArmBin* arm, const std::vector<u8>& data, u32 ramAddress, struct PatternMatches* patternMatches, int& arenaLoOut, u32& pointerValueOut)
{
    for (int switchCaseMatch : findPattern(data, patternMatches->switchCase, 0, data.size()))
    {
        bool foundReference = false;
        for (const auto& refPattern : patternMatches->reference)
        {
            auto result = std::search(
                data.begin() + switchCaseMatch, data.begin() + switchCaseMatch + 0x100,
                refPattern.begin(), refPattern.end()
            );
            if (result != data.begin() + switchCaseMatch + 0x100)
            {
                foundReference = true;
                break;
            }
        }
        if (foundReference)
            continue;
        for (int ldrMatch : findPattern(data, patternMatches->ldr, switchCaseMatch, switchCaseMatch + 0x50))
        {
            ldrMatch -= 1;
            u32 ldrAddress = ramAddress + ldrMatch;
            if (arm->sanityCheckAddress(ldrAddress))
            {
                u32 offset = 0;
                if (patternMatches == &patternMatchesThumb)
                {
                    offset = ldrAddress - ramAddress;
                    offset = ((offset + 4) & ~0x3) + ((data[offset] & 0xFF) << 2);
                }
                else
                {
                    const std::vector<u8> ldmia_buffer(data.begin() + ldrMatch + 4, data.begin() + ldrMatch + 8);
                    bool foundLdmiaPattern = false;
                    for (const auto& pattern : patternMatches->ldmia)
                    {
                        if (ldrMatch + 4 + pattern.size() <= data.size() &&
                            std::equal(pattern.begin(), pattern.end(), data.begin() + ldrMatch + 4))
                        {
                            foundLdmiaPattern = true;
                            break;
                        }
                    }
                    if (!foundLdmiaPattern)
                        continue;
                    offset = ldrAddress - ramAddress;
                    offset += data[offset] + 8;
                }
                u32 pointerValue = Util::read<u32>(&data[offset]);
                if (arm->sanityCheckAddress(pointerValue))
                {
                    arenaLoOut = ramAddress + offset;
					pointerValueOut = pointerValue;
                    return true;
                }
            }
        }
    }
    return false;
}

void findArenaLo(ArmBin* arm, int& arenaLoOut, u32& newcodeDestOut)
{
	std::vector<u8>& data = arm->data();

	u32 armRamAddress = arm->getRamAddress();
	u32 autoloadStart = arm->getModuleParams()->autoloadStart;
	std::vector<u8> subset(data.begin(), data.begin() + (autoloadStart - armRamAddress));
	if (ArenaLoFinder::processMatches(arm, subset, armRamAddress, &ArenaLoFinder::patternMatchesArm, arenaLoOut, newcodeDestOut) ||
		ArenaLoFinder::processMatches(arm, subset, armRamAddress, &ArenaLoFinder::patternMatchesThumb, arenaLoOut, newcodeDestOut))
		return;

	for (const ArmBin::AutoLoadEntry& autoload : arm->getAutoloadList())
	{
		std::vector<u8> subset(data.begin() + autoload.dataOff, data.begin() + autoload.dataOff + autoload.size);
		if (ArenaLoFinder::processMatches(arm, subset, autoload.address, &ArenaLoFinder::patternMatchesArm, arenaLoOut, newcodeDestOut) ||
			ArenaLoFinder::processMatches(arm, subset, autoload.address, &ArenaLoFinder::patternMatchesThumb, arenaLoOut, newcodeDestOut))
			return;
	}

	std::ostringstream oss;
	oss << "Failed to find " << OSTR("arenaLo") << " and no valid " << OSTR("arenaLo") << " was provided.";
	throw ncp::exception(oss.str());
}

}
