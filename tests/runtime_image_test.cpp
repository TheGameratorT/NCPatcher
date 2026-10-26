// The runtime module image: the relocations found by comparing two links, the
// header, and the hook bridge that runs wherever a module is loaded.

#include "../source/patch/runtime_image.hpp"
#include "../source/patch/asm_generator.hpp"

#include <cstdio>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

using namespace ncp::patch;

static bool passed = true;

static void check(bool condition, const std::string& what)
{
	if (!condition)
	{
		std::cout << "FAIL: " << what << '\n';
		passed = false;
	}
}

static u32 read32(const u8* p)
{
	return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24);
}

static void put32(std::vector<u8>& out, u32 value)
{
	for (int i = 0; i < 4; i++)
		out.push_back(u8(value >> (i * 8)));
}

static std::string hex(u32 value)
{
	char buffer[16];
	std::snprintf(buffer, sizeof(buffer), "0x%08X", value);
	return buffer;
}

static void testRelocations()
{
	using namespace runtime;
	const u32 a = LinkBaseA;
	const u32 b = LinkBaseB;

	// Addresses inside the module move with it, everything else stays.
	std::vector<u8> imageA, imageB;
	const u32 wordsA[] = { a + 0x40, 0x02000000, a, 0x1234, a + 0x41 };
	const u32 wordsB[] = { b + 0x40, 0x02000000, b, 0x1234, b + 0x41 };
	for (u32 w : wordsA) put32(imageA, w);
	for (u32 w : wordsB) put32(imageB, w);

	std::vector<u32> relocs, mismatches;
	findRelocations(imageA, a, imageB, b, relocs, mismatches);
	check(relocs == std::vector<u32>{ 0, 8, 16 }, "relocations of a clean pair");
	check(mismatches.empty(), "no mismatches in a clean pair");

	// A word that changes by anything other than the distance between the
	// bases is not an address the loader can fix.
	imageA.clear();
	imageB.clear();
	put32(imageA, a >> 1);
	put32(imageB, b >> 1);
	put32(imageA, a + 4);
	put32(imageB, b + 4);
	imageA.push_back(1);
	imageB.push_back(2);
	relocs.clear();
	findRelocations(imageA, a, imageB, b, relocs, mismatches);
	check(relocs == std::vector<u32>{ 4 }, "relocation next to a mismatch");
	check(mismatches == std::vector<u32>{ 0, 8 }, "mismatches of a shifted word and a trailing byte");

	// An address stored at an unaligned offset.
	std::vector<u8> unalignedA(12, 0), unalignedB(12, 0);
	for (int i = 0; i < 4; i++)
	{
		unalignedA[2 + i] = u8((a + 0x10) >> (i * 8));
		unalignedB[2 + i] = u8((b + 0x10) >> (i * 8));
	}
	relocs.clear();
	mismatches.clear();
	findRelocations(unalignedA, a, unalignedB, b, relocs, mismatches);
	check(relocs.empty() && !mismatches.empty(), "an unaligned address is a mismatch");
}

static void testHeader()
{
	using namespace runtime;

	ModuleHeader header;
	header.alignLog2 = 3;
	header.base = LinkBaseA;
	header.size = 0x100;
	header.exports = LinkBaseA + 0x40;
	header.patches = LinkBaseA + 0x80;
	header.patchCount = 3;
	header.relocs = LinkBaseA + 0x100;
	header.init = LinkBaseA + 0x60;
	header.initCount = 2;
	header.fini = LinkBaseA + 0x68;
	header.finiCount = 1;
	header.atexit = LinkBaseA + 0xC0;
	header.atexitCapacity = 5;

	std::vector<u8> image(0x100, 0);
	writeModuleHeader(image, header);
	check(read32(image.data()) == ModuleMagic, "magic");
	check(image[4] == ModuleVersion && image[5] == 0, "version");
	check(image[6] == 0 && image[7] == 3, "flags and alignment");
	check(read32(image.data() + 20) == LinkBaseA + 0x80, "patch table at offset 20");
	check(image[24] == 3 && image[25] == 0 && image[26] == 0 && image[27] == 0, "patch count, applied and flags");
	check(read32(image.data() + 48) == 1, "fini count at offset 48");
	check(read32(image.data() + 52) == LinkBaseA + 0xC0, "destructor table at offset 52");
	check(read32(image.data() + 56) == 5, "destructor capacity and count");

	appendRelocations(image, { 8, 16, 20 });
	check(image.size() == 0x100 + 12, "relocation table appended");
	check(read32(image.data() + 0x100 + 4) == 16, "relocation table contents");

	ModuleHeader back = readModuleHeader(image);
	check(back.alignLog2 == 3, "alignment read back");
	check(back.relocCount == 3, "relocation count written");
	check(back.base == header.base && back.size == header.size && back.exports == header.exports &&
	      back.patches == header.patches && back.patchCount == header.patchCount &&
	      back.relocs == header.relocs && back.init == header.init && back.initCount == header.initCount &&
	      back.fini == header.fini && back.finiCount == header.finiCount &&
	      back.atexit == header.atexit && back.atexitCapacity == header.atexitCapacity, "header round trip");
}

// The words of a bridge built for the instruction `op` at `og`, with the
// bridge at `bridge` calling the ARM function at `src`.
static std::vector<u32> bridgeFor(u32 op, u32 og = 0x02000000, bool arm9 = true)
{
	const u32 bridge = 0x10000100;
	const u32 src = 0x10000000;
	u8 out[AsmGenerator::SizeOfModuleHookBridge];
	AsmGenerator::makeModuleHookBridge(out, bridge, src, false, op, og, arm9);

	std::vector<u32> words;
	for (u32 i = 0; i < AsmGenerator::SizeOfModuleHookBridge; i += 4)
		words.push_back(read32(out + i));

	check(words[0] == AsmGenerator::armHookPush, hex(op) + ": push");
	check(words[1] == AsmGenerator::makeJumpOpCode(AsmGenerator::armOpcodeBL, bridge + 4, src), hex(op) + ": call");
	check(words[2] == AsmGenerator::armHookPop, hex(op) + ": pop");
	check(words[7] == og + 4, hex(op) + ": return literal");
	return words;
}

static void expectBridge(u32 op, u32 s0, u32 s1, u32 s2, u32 target, const char* what)
{
	std::vector<u32> w = bridgeFor(op);
	const std::string name = std::string(what) + " (" + hex(op) + ")";
	check(w[3] == s0, name + ": first slot is " + hex(w[3]) + ", expected " + hex(s0));
	check(w[4] == s1, name + ": second slot is " + hex(w[4]) + ", expected " + hex(s1));
	check(w[5] == s2, name + ": third slot is " + hex(w[5]) + ", expected " + hex(s2));
	check(w[6] == target, name + ": target literal is " + hex(w[6]) + ", expected " + hex(target));
}

static void expectRefused(u32 op, const char* what, bool arm9 = true)
{
	bool threw = false;
	try { bridgeFor(op, 0x02000000, arm9); }
	catch (const std::exception&) { threw = true; }
	check(threw, std::string(what) + " (" + hex(op) + ") should be refused");
}

static void testHookBridge()
{
	// LDR PC, [PC, #imm] from each slot to the return literal, and LDR Rd
	// from the first slot to the target literal.
	const u32 RetFrom12 = 0xE59FF008;
	const u32 RetFrom16 = 0xE59FF004;
	const u32 RetFrom20 = 0xE59FF000;
	const u32 og = 0x02000000;

	expectBridge(0xE1A00001, 0xE1A00001, RetFrom16, RetFrom20, 0, "mov r0, r1");
	expectBridge(0xE8BD8010, 0xE8BD8010, RetFrom16, RetFrom20, 0, "pop {r4, pc}");
	expectBridge(0xE12FFF1E, 0xE12FFF1E, RetFrom16, RetFrom20, 0, "bx lr");
	expectBridge(0xE10F0000, 0xE10F0000, RetFrom16, RetFrom20, 0, "mrs r0, cpsr");

	// Branches load their destination, calls the return address first.
	expectBridge(0xEA00003E, 0xE59FF004, RetFrom16, RetFrom20, og + 0x100, "b");
	expectBridge(0x1A00003E, 0x159FF004, RetFrom16, RetFrom20, og + 0x100, "bne");
	expectBridge(0x0BFFFFFE, 0x059FE008, 0x059FF000, RetFrom20, og, "bleq");
	expectBridge(0xFB000000, 0xE59FE008, 0xE59FF000, RetFrom20, og + 0xB, "blx");

	// Loads from the PC load the address, then load from it.
	expectBridge(0xE59F2010, 0xE59F2004, 0xE5922000, RetFrom20, og + 0x18, "ldr r2, [pc, #16]");
	expectBridge(0xE51F3008, 0xE59F3004, 0xE5933000, RetFrom20, og, "ldr r3, [pc, #-8]");
	expectBridge(0xE5DF1004, 0xE59F1004, 0xE5D11000, RetFrom20, og + 0xC, "ldrb r1, [pc, #4]");
	expectBridge(0xE1DF11B2, 0xE59F1004, 0xE1D110B0, RetFrom20, og + 0x1A, "ldrh r1, [pc, #18]");
	expectBridge(0x01DF10D2, 0x059F1004, 0x01D110D0, RetFrom20, og + 0xA, "ldrsbeq r1, [pc, #2]");

	// ADR loads the address it computes, and into the PC jumps there.
	expectBridge(0xE28F0020, 0xE59F0004, RetFrom16, RetFrom20, og + 0x28, "add r0, pc, #32");
	expectBridge(0xE24F0004, 0xE59F0004, RetFrom16, RetFrom20, og + 4, "sub r0, pc, #4");
	expectBridge(0xE28FF008, 0xE59FF004, RetFrom16, RetFrom20, og + 0x10, "add pc, pc, #8");

	(void)RetFrom12;

	expectRefused(0xE1A0000F, "mov r0, pc");
	expectRefused(0xE080000F, "add r0, r0, pc");
	expectRefused(0xE08F0001, "add r0, pc, r1");
	expectRefused(0xE79F0001, "ldr r0, [pc, r1]");
	expectRefused(0xE791000F, "ldr r0, [r1, pc]");
	expectRefused(0xE58F0004, "str r0, [pc, #4]");
	expectRefused(0xE59FF004, "ldr pc, [pc, #4]");
	expectRefused(0xE5BF0004, "ldr r0, [pc, #4]!");
	expectRefused(0xE89F000F, "ldm pc, {r0-r3}");
	expectRefused(0xE92D8000, "push {pc}");
	expectRefused(0xE12FFF1F, "bx pc");
	expectRefused(0xE29F0004, "adds r0, pc, #4");
	expectRefused(0xE1CF00B4, "strh r0, [pc, #4]");
	expectRefused(0xFB000000, "blx on the ARM7", false);
}

int main()
{
	testRelocations();
	testHeader();
	testHookBridge();

	if (passed)
		std::cout << "all checks passed\n";
	return passed ? 0 : 1;
}
