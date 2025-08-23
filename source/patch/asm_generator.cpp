#include "asm_generator.hpp"

#include <sstream>

#include "../except.hpp"
#include "../util.hpp"

u32 AsmGenerator::makeJumpOpCode(u32 opCode, u32 fromAddr, u32 toAddr)
{
    s32 offset = (s32(toAddr) - s32(fromAddr)) >> 2;
    offset -= 2;
    
    // Check ARM BL/B range: ±32MB (±0x800000 instructions * 4 bytes = ±0x2000000 bytes)
    if (offset < -0x800000 || offset > 0x7FFFFF) {
        std::ostringstream oss;
        oss << "ARM BL/B instruction offset out of range: " << std::uppercase << std::hex 
            << "0x" << fromAddr << " -> 0x" << toAddr 
            << " (offset: " << std::dec << (offset * 4) << " bytes)";
        throw ncp::exception(oss.str());
    }
    
    return opCode | (u32(offset) & 0xFFFFFF);
}

u32 AsmGenerator::makeBLXOpCode(u32 fromAddr, u32 toAddr)
{
    // BLX (immediate) instruction encoding for ARMv5TE
    // Target must be halfword aligned but can be ARM or THUMB
    if (toAddr & 1) {
        std::ostringstream oss;
        oss << "BLX target address must be halfword aligned: from 0x"
            << std::uppercase << std::hex << fromAddr << " to 0x" << toAddr;
        throw ncp::exception(oss.str());
    }
    
    s32 offset = s32(toAddr) - s32(fromAddr) - 8;
    
    // Check BLX range: ±32MB (±0x2000000 bytes)
    if (offset < -0x2000000 || offset > 0x1FFFFFF) {
        std::ostringstream oss;
        oss << "ARM BLX instruction offset out of range: " << std::uppercase << std::hex 
            << "0x" << fromAddr << " -> 0x" << toAddr 
            << " (offset: " << std::dec << offset << " bytes)";
        throw ncp::exception(oss.str());
    }
    
    // Extract H bit (bit 1 of offset after alignment)
    u32 h = (offset & 2) >> 1;
    
    // Calculate 24-bit immediate (offset >> 2)
    u32 imm24 = (offset >> 2) & 0xFFFFFF;
    
    // BLX encoding: 1111 101 H imm24
    return 0xFA000000 | (h << 24) | imm24;
}

u32 AsmGenerator::makeThumbCallOpCode(bool exchange, u32 fromAddr, u32 toAddr)
{
    s32 offset;
    
    if (exchange) {
        // BLX: target is always ARM (word-aligned), fromAddr alignment doesn't matter for calculation
        // Target address must be word-aligned
        if (toAddr & 3) {
            std::ostringstream oss;
            oss << "BLX target address must be word-aligned: 0x" << std::uppercase << std::hex << toAddr;
            throw ncp::exception(oss.str());
        }
        offset = (s32(toAddr) - s32(fromAddr)) >> 1;
    } else {
        // BL: both addresses are THUMB (halfword-aligned)
        offset = (s32(toAddr) - s32(fromAddr)) >> 1;
    }
    offset -= 2;
    
    // Check THUMB BL/BLX range: ±16MB (±0x400000 instructions * 2 bytes = ±0x800000 bytes)
    if (offset < -0x400000 || offset > 0x3FFFFF) {
        std::ostringstream oss;
        oss << "THUMB BL/BLX instruction offset out of range: " << std::uppercase << std::hex 
            << "0x" << fromAddr << " -> 0x" << toAddr 
            << " (offset: " << std::dec << (offset * 2) << " bytes)";
        throw ncp::exception(oss.str());
    }
    
    u16 opcode0 = thumbOpCodeBL0 | ((offset & 0x7FF800) >> 11);
    u16 opcode1 = (exchange ? thumbOpCodeBLX1 : thumbOpCodeBL1) | (offset & 0x7FF);
    return (u32(opcode1) << 16) | opcode0;
}

u32 AsmGenerator::fixupOpCode(u32 opCode, u32 ogAddr, u32 newAddr)
{
    // TODO: check for other relative instructions other than B and BL, like LDR
    if (((opCode >> 25) & 0b111) == 0b101)
    {
        u32 opCodeBase = opCode & 0xFF000000;
        u32 toAddr = (((opCode & 0xFFFFFF) + 2) << 2) + ogAddr;
        return makeJumpOpCode(opCodeBase, newAddr, toAddr);
    }
    return opCode;
}
