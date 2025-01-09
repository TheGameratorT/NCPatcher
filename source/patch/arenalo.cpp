#include "arenalo.hpp"

#include <iostream>
#include <algorithm>

namespace ArenaLoFinder {

static const std::vector<int> findPattern(const std::vector<u8>& data, const std::vector<u8>& pattern, u32 start, s32 end)
{
    std::vector<int> matches;
    size_t pattern_length = pattern.size();
    for (size_t i = start; i < end - pattern_length + 1; i++)
    {
        if (std::equal(pattern.begin(), pattern.end(), data.begin() + i))
        {
            matches.push_back(i);
        }
    }
    return matches;
}

// Searches in the arm9 binary for OS_GetInitArenaLo, and finds the address of arenaLo
bool processMatches(ArmBin *arm9, const std::vector<u8>& data, u32 ramAddress, BuildTarget* target, struct PatternMatches* patternMatches)
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
            if (arm9->sanityCheckAddress(ldrAddress))
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
                u32 pointerValue = *reinterpret_cast<const u32*>(&data[offset]);
                if (arm9->sanityCheckAddress(pointerValue))
                {
                    target->arenaLo = ramAddress + offset;
                    return true;
                }
            }
        }
    }
    return false;
}

}
