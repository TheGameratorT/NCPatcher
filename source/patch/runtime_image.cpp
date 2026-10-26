#include "runtime_image.hpp"

#include "../utils/endian.hpp"

namespace ncp::patch::runtime {

void writeModuleHeader(std::span<u8> image, const ModuleHeader& header)
{
	le::writeU32(image, 0, ModuleMagic);
	le::writeU16(image, 4, ModuleVersion);
	image[6] = 0; // flags
	image[7] = header.alignLog2;
	le::writeU32(image, 8, header.base);
	le::writeU32(image, 12, header.size);
	le::writeU32(image, 16, header.exports);
	le::writeU32(image, 20, header.patches);
	le::writeU16(image, 24, header.patchCount);
	image[26] = 0; // applied
	image[27] = 0; // flags
	le::writeU32(image, 28, header.relocs);
	le::writeU32(image, 32, header.relocCount);
	le::writeU32(image, 36, header.init);
	le::writeU32(image, 40, header.initCount);
	le::writeU32(image, 44, header.fini);
	le::writeU32(image, 48, header.finiCount);
	le::writeU32(image, 52, header.atexit);
	le::writeU16(image, 56, header.atexitCapacity);
	le::writeU16(image, 58, 0); // atexitCount
}

ModuleHeader readModuleHeader(std::span<const u8> image)
{
	ModuleHeader header;
	header.alignLog2 = image[7];
	header.base = le::readU32(image, 8);
	header.size = le::readU32(image, 12);
	header.exports = le::readU32(image, 16);
	header.patches = le::readU32(image, 20);
	header.patchCount = le::readU16(image, 24);
	header.relocs = le::readU32(image, 28);
	header.relocCount = le::readU32(image, 32);
	header.init = le::readU32(image, 36);
	header.initCount = le::readU32(image, 40);
	header.fini = le::readU32(image, 44);
	header.finiCount = le::readU32(image, 48);
	header.atexit = le::readU32(image, 52);
	header.atexitCapacity = le::readU16(image, 56);
	return header;
}

void findRelocations(
	std::span<const u8> imageA, u32 baseA,
	std::span<const u8> imageB, u32 baseB,
	std::vector<u32>& relocs, std::vector<u32>& mismatches
)
{
	const u32 delta = baseB - baseA;
	const std::size_t size = imageA.size() < imageB.size() ? imageA.size() : imageB.size();

	// A word is compared whole, at word offsets only. An address stored at an
	// unaligned offset differs in two words, by amounts that are not the
	// delta, and is reported rather than silently missed.
	std::size_t offset = 0;
	for (; offset + 4 <= size; offset += 4)
	{
		u32 a = le::readU32(imageA, offset);
		u32 b = le::readU32(imageB, offset);
		if (a == b)
			continue;
		(b - a == delta ? relocs : mismatches).push_back(u32(offset));
	}

	for (; offset < size; offset++)
	{
		if (imageA[offset] != imageB[offset])
		{
			mismatches.push_back(u32(offset & ~3u));
			break;
		}
	}
}

void appendRelocations(std::vector<u8>& image, const std::vector<u32>& relocs)
{
	le::writeU32(image, 32, u32(relocs.size()));

	std::size_t offset = image.size();
	image.resize(offset + relocs.size() * 4);
	for (u32 reloc : relocs)
	{
		le::writeU32(image, offset, reloc);
		offset += 4;
	}
}

} // namespace ncp::patch::runtime
