#pragma once

#include "../utils/types.hpp"

namespace ncp::patch {

class AsmGenerator
{
public:
    static u32 makeJumpOpCode(u32 opCode, u32 fromAddr, u32 toAddr);
    static u32 makeBLXOpCode(u32 fromAddr, u32 toAddr);
    static u32 makeThumbCallOpCode(bool exchange, u32 fromAddr, u32 toAddr);
    static u32 fixupOpCode(u32 opCode, u32 ogAddr, u32 newAddr, bool isArm9 = true);

    // ARM opcodes
    static constexpr u32 armOpcodeB = 0xEA000000; // B
    static constexpr u32 armOpcodeBL = 0xEB000000; // BL
    static constexpr u32 armOpCodeBLX = 0xFA000000; // BLX
    static constexpr u32 armHookPush = 0xE92D500F; // PUSH {R0-R3,R12,LR}
    static constexpr u32 armHookPop = 0xE8BD500F; // POP {R0-R3,R12,LR}
    
    // THUMB opcodes
    static constexpr u16 thumbOpCodeBL0 = 0xF000; // BL
    static constexpr u16 thumbOpCodeBL1 = 0xF800; // <BL>
    static constexpr u16 thumbOpCodeBLX1 = 0xE800; // <BL>X
    static constexpr u16 thumbOpCodePushLR = 0xB500; // PUSH {LR}
    static constexpr u16 thumbOpCodePopPC = 0xBD00; // POP {PC}

private:
    AsmGenerator() = delete;
};

} // namespace ncp::patch
