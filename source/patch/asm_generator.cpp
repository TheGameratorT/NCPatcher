#include "asm_generator.hpp"

#include <sstream>

#include "../system/except.hpp"
#include "../utils/util.hpp"

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

u32 AsmGenerator::fixupOpCode(u32 opCode, u32 ogAddr, u32 newAddr, bool isArm9)
{
    /*
     * ARM Instruction Relocation Support for In-Place Fixups
     * 
     * Compatible with:
     * - ARMv4T (ARM7TDMI): Basic ARM instructions, no BLX
     * - ARMv5TE (ARM946E-S): All ARMv4T + BLX, enhanced DSP
     * 
     * Supported for relocation:
     * - B/BL (branch/branch-link) - bits[27:25] = 101
     * - BLX (branch-link-exchange) - ARMv5TE+ only 
     * - LDR/STR with PC-relative addressing - bits[27:25] = 010
     * - ADR pseudo-instruction (ADD/SUB Rd,PC,#imm) - bits[27:25] = 001
     * - LDRH/STRH/LDRSB/LDRSH with PC-relative - bits[27:25] = 000, bits[7,4] = 11
     * 
     * Not supported (throws error):
     * - LDM/STM with PC as base register (complex multi-word fixup needed)
     * - Coprocessor instructions with PC-relative addressing 
     * - Instructions with post-indexing or write-back modes
     * - Register-offset addressing modes (only immediate offsets supported)
     * - Any instruction requiring more than 4 bytes or multiple instructions
     */
    // Decode instruction type based on ARM instruction encoding
    u32 bits25_27 = (opCode >> 25) & 0b111;
    u32 bits20_24 = (opCode >> 20) & 0b11111;
    u32 bits4_7 = (opCode >> 4) & 0b1111;
    
    // Branch instructions (B, BL) - bits [27:25] = 101
    if (bits25_27 == 0b101)
    {
        u32 opCodeBase = opCode & 0xFF000000;
        u32 linkBit = (opCode >> 24) & 1;
        
        // Check for BLX (immediate) - only available in ARMv5TE+ (ARM9)
        // BLX(1) has bits [27:25] = 101 and bit [24] = 1, plus H bit in [24]
        if ((opCode & 0xFE000000) == 0xFA000000) // BLX immediate encoding
        {
            if (!isArm9) // ARMv4T (ARM7) doesn't support BLX
            {
                std::ostringstream oss;
                oss << "Cannot relocate BLX instruction at 0x" << std::hex << std::uppercase << ogAddr 
                    << " - BLX not available in ARMv4T (ARM7), use BL + BX sequence instead";
                throw ncp::exception(oss.str());
            }
            // For ARM9 (ARMv5TE), BLX can be handled like normal branches
        }
        
        // Standard B/BL instructions
        // Extract 24-bit signed offset and properly sign-extend it
        s32 offset = s32(opCode << 8) >> 8; // Sign-extend by shifting left then right
        u32 toAddr = u32((offset + 2) * 4 + s32(ogAddr));
        return makeJumpOpCode(opCodeBase, newAddr, toAddr);
    }
    
    // Single Data Transfer (LDR/STR with PC-relative addressing) - bits [27:25] = 010
    if (bits25_27 == 0b010)
    {
        u32 baseReg = (opCode >> 16) & 0xF;
        
        // Only handle PC-relative loads/stores (Rn = PC = 15)
        if (baseReg == 15)
        {
            bool preIndex = (opCode >> 24) & 1;     // P bit
            bool addOffset = (opCode >> 23) & 1;    // U bit (add/subtract)
            bool byteAccess = (opCode >> 22) & 1;   // B bit
            bool writeBack = (opCode >> 21) & 1;    // W bit
            bool loadStore = (opCode >> 20) & 1;    // L bit (1=load, 0=store)
            
            // Reject unsupported combinations that would be complex to relocate
            if (!preIndex || writeBack)
            {
                std::ostringstream oss;
                oss << "Cannot relocate LDR/STR instruction with post-indexing or write-back at 0x" 
                    << std::hex << std::uppercase << ogAddr;
                throw ncp::exception(oss.str());
            }
            
            u32 offset = opCode & 0xFFF; // 12-bit immediate offset
            
            // Calculate the absolute target address
            u32 targetAddr;
            if (addOffset) {
                targetAddr = ogAddr + 8 + offset; // PC is 8 bytes ahead in ARM mode
            } else {
                targetAddr = ogAddr + 8 - offset;
            }
            
            // Calculate new offset from new location
            s32 newOffset = s32(targetAddr) - s32(newAddr + 8);
            
            // Check if new offset fits in 12-bit signed range
            if (newOffset < -4095 || newOffset > 4095)
            {
                std::ostringstream oss;
                oss << "PC-relative LDR/STR offset out of range after relocation: " 
                    << std::dec << newOffset << " bytes (max ±4095) at 0x" 
                    << std::hex << std::uppercase << ogAddr;
                throw ncp::exception(oss.str());
            }
            
            // Reconstruct the instruction with new offset
            u32 newOpCode = opCode & 0xFF000000; // Preserve condition and opcode
            newOpCode |= (opCode & 0x00F00000);  // Preserve other bits
            
            if (newOffset >= 0) {
                newOpCode |= (1 << 23);          // Set U bit (add)
                newOpCode |= u32(newOffset) & 0xFFF;
            } else {
                newOpCode &= ~(1 << 23);         // Clear U bit (subtract)
                newOpCode |= u32(-newOffset) & 0xFFF;
            }
            
            return newOpCode;
        }
    }
    
    // ADR pseudo-instruction (ADD/SUB with PC as source) - bits [27:25] = 001
    if (bits25_27 == 0b001)
    {
        u32 srcReg = (opCode >> 16) & 0xF;
        u32 opcodeBits = (opCode >> 21) & 0xF;
        
        // Check for ADD Rd, PC, #imm or SUB Rd, PC, #imm (ADR pseudo-instruction)
        if (srcReg == 15 && (opcodeBits == 0x4 || opcodeBits == 0x2)) // ADD or SUB
        {
            bool isAdd = (opcodeBits == 0x4);
            u32 rotateImm = (opCode >> 8) & 0xF;
            u32 immediate = opCode & 0xFF;
            
            // Rotate the 8-bit immediate by 2*rotate_imm positions
            u32 offset = (immediate >> (rotateImm * 2)) | (immediate << (32 - rotateImm * 2));
            
            // Calculate target address
            u32 targetAddr;
            if (isAdd) {
                targetAddr = ogAddr + 8 + offset;
            } else {
                targetAddr = ogAddr + 8 - offset;
            }
            
            // Calculate new offset
            s32 newOffset = s32(targetAddr) - s32(newAddr + 8);
            
            // Try to encode as ADD Rd, PC, #newOffset
            if (newOffset >= 0)
            {
                // Try to find a rotation that works for the positive offset
                for (u32 rot = 0; rot < 16; rot += 2)
                {
                    u32 testVal = u32(newOffset);
                    u32 rotated = (testVal << (rot)) | (testVal >> (32 - rot));
                    if ((rotated & ~0xFF) == 0)
                    {
                        u32 newOpCode = (opCode & 0xFFF00000) | (rot << 8) | rotated;
                        newOpCode |= (0x4 << 21); // Ensure it's ADD
                        return newOpCode;
                    }
                }
            }
            else
            {
                // Try to encode as SUB Rd, PC, #(-newOffset)
                u32 absOffset = u32(-newOffset);
                for (u32 rot = 0; rot < 16; rot += 2)
                {
                    u32 rotated = (absOffset << rot) | (absOffset >> (32 - rot));
                    if ((rotated & ~0xFF) == 0)
                    {
                        u32 newOpCode = (opCode & 0xFFF00000) | (rot << 8) | rotated;
                        newOpCode &= ~(0xF << 21);
                        newOpCode |= (0x2 << 21); // Set to SUB
                        return newOpCode;
                    }
                }
            }
            
            // If we can't encode the offset, throw an error
            std::ostringstream oss;
            oss << "Cannot relocate ADR instruction - offset " << std::dec << newOffset 
                << " cannot be encoded as ARM immediate at 0x" << std::hex << std::uppercase << ogAddr;
            throw ncp::exception(oss.str());
        }
    }
    
    // Check for other PC-relative instructions that cannot be relocated in-place
    // These would require complex fixups or multiple instructions
    
    // Block Data Transfer (LDM/STM) with PC - bits [27:25] = 100
    if (bits25_27 == 0b100)
    {
        u32 baseReg = (opCode >> 16) & 0xF;
        if (baseReg == 15)
        {
            std::ostringstream oss;
            oss << "Cannot relocate LDM/STM instruction with PC as base register at 0x" 
                << std::hex << std::uppercase << ogAddr << " - requires complex fixup";
            throw ncp::exception(oss.str());
        }
    }
    
    // Halfword Data Transfer instructions - bits [27:25] = 000, bits [7,5,4] = 1x1
    // These include LDRH, STRH, LDRSB, LDRSH (available in ARMv4+)
    if (bits25_27 == 0b000 && (bits4_7 & 0b1001) == 0b1001)
    {
        u32 baseReg = (opCode >> 16) & 0xF;
        if (baseReg == 15)
        {
            // Similar to LDR/STR handling but with different offset encoding
            bool preIndex = (opCode >> 24) & 1;     // P bit
            bool addOffset = (opCode >> 23) & 1;    // U bit (add/subtract)
            bool immForm = (opCode >> 22) & 1;      // I bit (immediate vs register)
            bool writeBack = (opCode >> 21) & 1;    // W bit
            bool loadStore = (opCode >> 20) & 1;    // L bit (1=load, 0=store)
            
            if (!preIndex || writeBack || !immForm)
            {
                std::ostringstream oss;
                oss << "Cannot relocate halfword transfer instruction with post-indexing, write-back, or register offset at 0x" 
                    << std::hex << std::uppercase << ogAddr;
                throw ncp::exception(oss.str());
            }
            
            // For immediate form: offset is bits[11:8,3:0] (8-bit)
            u32 offsetHi = (opCode >> 8) & 0xF;
            u32 offsetLo = opCode & 0xF;
            u32 offset = (offsetHi << 4) | offsetLo;
            
            // Calculate target address
            u32 targetAddr;
            if (addOffset) {
                targetAddr = ogAddr + 8 + offset;
            } else {
                targetAddr = ogAddr + 8 - offset;
            }
            
            // Calculate new offset
            s32 newOffset = s32(targetAddr) - s32(newAddr + 8);
            
            // Check if new offset fits in 8-bit range
            if (newOffset < -255 || newOffset > 255)
            {
                std::ostringstream oss;
                oss << "PC-relative halfword transfer offset out of range after relocation: " 
                    << std::dec << newOffset << " bytes (max ±255) at 0x" 
                    << std::hex << std::uppercase << ogAddr;
                throw ncp::exception(oss.str());
            }
            
            // Reconstruct the instruction with new offset
            u32 newOpCode = opCode & 0xFFF00F00; // Preserve everything except offset bits
            
            u32 absOffset;
            if (newOffset >= 0) {
                newOpCode |= (1 << 23);          // Set U bit (add)
                absOffset = u32(newOffset);
            } else {
                newOpCode &= ~(1 << 23);         // Clear U bit (subtract)
                absOffset = u32(-newOffset);
            }
            
            // Split offset into high and low nibbles
            newOpCode |= ((absOffset & 0xF0) << 4) | (absOffset & 0xF);
            
            return newOpCode;
        }
    }
    
    // Coprocessor instructions with PC - bits [27:25] = 110/111
    if ((bits25_27 == 0b110 || bits25_27 == 0b111))
    {
        u32 baseReg = (opCode >> 16) & 0xF;
        if (baseReg == 15)
        {
            std::ostringstream oss;
            oss << "Cannot relocate coprocessor instruction with PC-relative addressing at 0x" 
                << std::hex << std::uppercase << ogAddr << " - not supported";
            throw ncp::exception(oss.str());
        }
    }
    
    // If no PC-relative addressing detected, return the instruction unchanged
    return opCode;
}
