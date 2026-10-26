#pragma once

#include <span>
#include <string>
#include <vector>

#include "../utils/types.hpp"

// The image of a runtime module (.ncrt), as ncprt.c loads it.
//
// A module is linked twice, at two bases far from anything the game maps, and
// the two images are compared: a word that differs holds an address inside the
// module, and is listed in the relocation table so the loader can add the
// address it was loaded at. Everything else is the same at any base, which
// includes every address of the game itself.
//
// The image starts with an ncprt_module header (ncp.h), little-endian:
//
//    0  u32 magic        'NCRT'
//    4  u16 version      ModuleVersion
//    6  u16 flags        0
//    8  u32 base         the address the image is relocated for
//   12  u32 size         bytes of code, data and zeroed bss
//   16  u32 exports      the ncprt_exports object, or 0
//   20  ncprt_group      the module's patches: table, count, applied, flags
//   28  u32 relocs       the relocation table, right after `size` bytes
//   32  u32 relocCount
//   36  u32 init         .init_array
//   40  u32 initCount
//   44  u32 fini         .fini_array
//   48  u32 finiCount
//
// Every pointer in the header is an address for `base`, and is itself in the
// relocation table, so after relocation the header describes the image where
// it now is, and the image can be relocated again from there.

namespace ncp::patch::runtime {

constexpr u32 ModuleMagic = 0x5452434E; // "NCRT"
constexpr u16 ModuleVersion = 1;
constexpr u32 SizeOfModuleHeader = 52;

// The bases the two links use. Both are far out of branch range of everything
// the DS maps, so the linker makes the same choices at either.
constexpr u32 LinkBaseA = 0x10000000;
constexpr u32 LinkBaseB = 0x20000000;

// The loader places images on this boundary, which is also a cache line.
constexpr u32 MaxModuleAlign = 32;

struct ModuleHeader
{
	u32 base = 0;
	u32 size = 0;
	u32 exports = 0;
	u32 patches = 0;
	u16 patchCount = 0;
	u32 relocs = 0;
	u32 relocCount = 0;
	u32 init = 0;
	u32 initCount = 0;
	u32 fini = 0;
	u32 finiCount = 0;
};

void writeModuleHeader(std::span<u8> image, const ModuleHeader& header);
ModuleHeader readModuleHeader(std::span<const u8> image);

// Compares the same image linked at `baseA` and at `baseB`. The offset of each
// word that differs by exactly baseB - baseA goes to `relocs`, and the offset
// of each that differs in any other way goes to `mismatches`, in both cases in
// ascending order. The images must have the same size.
void findRelocations(
	std::span<const u8> imageA, u32 baseA,
	std::span<const u8> imageB, u32 baseB,
	std::vector<u32>& relocs, std::vector<u32>& mismatches
);

// Appends the relocation table to `image` and records its count in the header.
// The header's `relocs` must already point right after the image.
void appendRelocations(std::vector<u8>& image, const std::vector<u32>& relocs);

} // namespace ncp::patch::runtime
