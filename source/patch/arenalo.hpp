#include "../types.hpp"
#include "../ndsbin/armbin.hpp"
#include "../config/buildtarget.hpp"

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

bool processMatches(ArmBin *arm9, const std::vector<u8>& data, u32 ramAddress, BuildTarget* target, struct PatternMatches* patternMatches);

}
