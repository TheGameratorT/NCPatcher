// The runtime writes the same branches ncpatcher writes at build time, with an
// encoder of its own in sdk/ncprt.c. This compiles that file for the host and
// checks every kind against AsmGenerator, in both directions.

#include "../source/patch/asm_generator.hpp"

#include <cstdio>
#include <iostream>

#define __ncp_lang_c
extern "C" {
#include "../sdk/ncp.h"
#include "../sdk/ncprt.c"
}

using namespace ncp::patch;

static u32 read32(const unsigned char* p)
{
	return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24);
}

static u16 read16(const unsigned char* p)
{
	return u16(p[0] | (p[1] << 8));
}

int main()
{
	bool passed = true;
	auto check = [&passed](bool condition, const char* what, u32 from, u32 to) {
		if (!condition)
		{
			std::printf("FAIL: %s from 0x%08X to 0x%08X\n", what, from, to);
			passed = false;
		}
	};

	// Forward and backward, near and far, with addresses that are word aligned
	// and, for THUMB, only halfword aligned.
	const u32 sites[] = { 0x02000000, 0x02004002, 0x020A2CCC, 0x020A2CCE, 0x0215E7C4, 0x023FFFFC };
	const u32 targets[] = { 0x02000800, 0x02010004, 0x020A2CD0, 0x020C03C8, 0x02150000, 0x023A0000 };

	for (u32 site : sites)
	{
		for (u32 target : targets)
		{
			unsigned char out[8];
			const bool armSite = (site & 3) == 0;

			if (armSite)
			{
				check(__ncprt_encode(NCPRT_ARM_B, site, target, out) == 4, "ARM B size", site, target);
				check(read32(out) == AsmGenerator::makeJumpOpCode(AsmGenerator::armOpcodeB, site, target),
					"ARM B", site, target);

				check(__ncprt_encode(NCPRT_ARM_BL, site, target, out) == 4, "ARM BL size", site, target);
				check(read32(out) == AsmGenerator::makeJumpOpCode(AsmGenerator::armOpcodeBL, site, target),
					"ARM BL", site, target);

				// THUMB destinations: word aligned and halfword aligned, passed
				// with the THUMB bit set as the tables carry them.
				for (u32 thumb : { target, target + 2 })
				{
					check(__ncprt_encode(NCPRT_ARM_BLX, site, thumb | 1, out) == 4, "ARM BLX size", site, thumb);
					check(read32(out) == AsmGenerator::makeBLXOpCode(site, thumb), "ARM BLX", site, thumb);
				}

				check(__ncprt_encode(NCPRT_ARM_JUMP, site, target | 1, out) == 8, "ARM jump size", site, target);
				check(read32(out) == 0xE51FF004 && read32(out + 4) == (target | 1), "ARM jump", site, target);
			}

			for (u32 thumbSite : { site, site + 2 })
			{
				check(__ncprt_encode(NCPRT_THUMB_BL, thumbSite, target | 1, out) == 4, "THUMB BL size", thumbSite, target);
				check(read32(out) == AsmGenerator::makeThumbCallOpCode(false, thumbSite, target),
					"THUMB BL", thumbSite, target);

				check(__ncprt_encode(NCPRT_THUMB_BLX, thumbSite, target, out) == 4, "THUMB BLX size", thumbSite, target);
				check(read32(out) == AsmGenerator::makeThumbCallOpCode(true, thumbSite, target),
					"THUMB BLX", thumbSite, target);

				// The jump forms are laid out exactly as PatchMaker writes them.
				check(__ncprt_encode(NCPRT_THUMB_JUMP, thumbSite, target | 1, out) == 8, "THUMB jump size", thumbSite, target);
				check(read16(out) == AsmGenerator::thumbOpCodePushLR
					&& read32(out + 2) == AsmGenerator::makeThumbCallOpCode(false, thumbSite + 2, target)
					&& read16(out + 6) == AsmGenerator::thumbOpCodePopPC,
					"THUMB jump", thumbSite, target);

				check(__ncprt_encode(NCPRT_THUMB_JUMPX, thumbSite, target, out) == 8, "THUMB jumpx size", thumbSite, target);
				check(read16(out) == AsmGenerator::thumbOpCodePushLR
					&& read32(out + 2) == AsmGenerator::makeThumbCallOpCode(true, thumbSite + 2, target)
					&& read16(out + 6) == AsmGenerator::thumbOpCodePopPC,
					"THUMB jumpx", thumbSite, target);
			}
		}
	}

	{
		unsigned char out[8];
		check(__ncprt_encode(NCPRT_COPY, 0x02000000, 0x02000800, out) == 0, "COPY is not a branch", 0x02000000, 0x02000800);
		check(__ncprt_encode(0xFF, 0x02000000, 0x02000800, out) == 0, "unknown kind", 0x02000000, 0x02000800);
	}

	check(sizeof(ncprt_patch) == 16, "ncprt_patch size", 0, 0);

	if (passed)
		std::cout << "runtime_encode_test: all checks passed\n";
	return passed ? 0 : 1;
}
