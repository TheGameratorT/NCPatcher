// Tests for source/rom/** and the BLZ codec.
//
// The container tests build their own small ROM rather than needing a real one,
// so they run in CI. What they check is the part that has no compiler in it and
// is therefore allowed to be exact: a ROM that is loaded and saved with nothing
// staged must come back byte for byte, and one that is patched must still have
// a consistent header, FAT and overlay table afterwards.

#include "../source/rom/endian.hpp"
#include "../source/rom/dir_accessor.hpp"
#include "../source/rom/fat.hpp"
#include "../source/rom/header.hpp"
#include "../source/rom/nds_rom.hpp"
#include "../source/rom/nitro_fs.hpp"
#include "../source/rom/overlay_table.hpp"
#include "../source/formats/blz.hpp"
#include "../source/utils/crc.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace ncp::rom;
namespace fs = std::filesystem;

static int g_failures = 0;

static void check(bool condition, const std::string& what)
{
	if (!condition)
	{
		std::cout << "FAIL: " << what << "\n";
		g_failures++;
	}
}

// --- endianness ------------------------------------------------------------

static void testEndian()
{
	std::vector<u8> buffer(16, 0);
	std::span<u8> out(buffer);

	writeU32(out, 0, 0x89ABCDEF);
	check(buffer[0] == 0xEF && buffer[1] == 0xCD && buffer[2] == 0xAB && buffer[3] == 0x89,
		"writeU32 stores little-endian regardless of the host");
	check(readU32(buffer, 0) == 0x89ABCDEF, "readU32 round-trips");

	writeU24(out, 4, 0x123456);
	check(buffer[4] == 0x56 && buffer[5] == 0x34 && buffer[6] == 0x12, "writeU24 stores little-endian");
	check(readU24(buffer, 4) == 0x123456, "readU24 round-trips");
	check(buffer[7] == 0, "writeU24 leaves the fourth byte alone");

	writeU16(out, 8, 0xBEEF);
	check(readU16(buffer, 8) == 0xBEEF, "readU16 round-trips");
	writeU64(out, 8, 0x0123456789ABCDEFull);
	check(readU64(buffer, 8) == 0x0123456789ABCDEFull, "readU64 round-trips");

	// A truncated ROM has to produce a message, not a segfault.
	bool threw = false;
	try { (void)readU32(buffer, 13); } catch (const std::out_of_range&) { threw = true; }
	check(threw, "reading past the end throws");

	threw = false;
	try { (void)readU32(buffer, std::size_t(-1)); } catch (const std::out_of_range&) { threw = true; }
	check(threw, "an offset that would wrap the length check still throws");

	check(alignUp(0, 4) == 0 && alignUp(1, 4) == 4 && alignUp(4, 4) == 4 && alignUp(5, 512) == 512,
		"alignUp rounds up to the next multiple");
}

static void testCrc()
{
	// The standard CRC-16/MODBUS check value.
	const std::string input = "123456789";
	const std::span<const u8> bytes(reinterpret_cast<const u8*>(input.data()), input.size());
	check(Crc::modbus16(bytes) == 0x4B37, "CRC-16/MODBUS of \"123456789\" is 0x4B37");
	check(Crc::modbus16(std::span<const u8>()) == 0xFFFF, "the CRC of nothing is the initial value");
}

// --- tables ----------------------------------------------------------------

static void testOverlayTable()
{
	OverlayEntry entry;
	entry.overlayId = 58;
	entry.ramAddress = 0x023C0000;
	entry.ramSize = 0x1234;
	entry.bssSize = 0x40;
	entry.staticInitStart = 0x023C0100;
	entry.staticInitEnd = 0x023C0110;
	entry.fileId = 1802;
	entry.compressedSize = 0xABCDEF;   // the full 24 bits
	entry.flags = OverlayEntry::FlagCompressed;

	std::vector<u8> row(OverlayEntry::SIZE);
	entry.serialize(row);
	const OverlayEntry back = OverlayEntry::parse(row);

	check(back.overlayId == entry.overlayId && back.ramAddress == entry.ramAddress
		&& back.ramSize == entry.ramSize && back.bssSize == entry.bssSize
		&& back.staticInitStart == entry.staticInitStart && back.staticInitEnd == entry.staticInitEnd
		&& back.fileId == entry.fileId, "an overlay row round-trips");
	check(back.compressedSize == 0xABCDEF, "the 24-bit compressed size survives the byte boundary");
	check(back.flags == OverlayEntry::FlagCompressed && back.compressed(),
		"the flags byte is not swallowed by the size field");
	check(back.storedSize() == 0xABCDEF, "a compressed overlay stores its compressed length");

	entry.setCompressed(false);
	check(!entry.compressed() && entry.flags == 0, "clearing the flag leaves the others alone");

	bool threw = false;
	try { (void)OverlayTable::parse(std::vector<u8>(OverlayEntry::SIZE + 1)); }
	catch (const std::exception&) { threw = true; }
	check(threw, "a table that is not a whole number of rows is rejected");
}

static void testFat()
{
	Fat fat;
	check(fat.add(FatEntry{ 0x100, 0x200 }) == 0, "the first file gets id 0");
	check(fat.add(FatEntry{ 0x200, 0x200 }) == 1, "ids are handed out in order");

	const std::vector<u8> bytes = fat.serialize();
	check(bytes.size() == 2 * FatEntry::SIZE, "the table is two rows long");

	const Fat back = Fat::parse(bytes);
	check(back.size() == 2 && back.entries()[0].start == 0x100 && back.entries()[0].end == 0x200,
		"a FAT round-trips");
	check(back.entries()[0].size() == 0x100 && back.entries()[1].size() == 0,
		"an empty file has start == end");
}

// --- file name table -------------------------------------------------------

static NitroFs buildTree()
{
	// root/  data/ (dir)  readme.txt (file 0)
	// data/  a.bin (file 1)  b.bin (file 2)
	std::vector<u8> raw;
	// Built by hand so the parser is tested against bytes, not against our own
	// serializer's idea of what the bytes should be.
	const u16 dirCount = 2;
	auto put32 = [&](u32 v) { for (int i = 0; i < 4; i++) raw.push_back(u8((v >> (8 * i)) & 0xFF)); };
	auto put16 = [&](u16 v) { raw.push_back(u8(v & 0xFF)); raw.push_back(u8((v >> 8) & 0xFF)); };

	const u32 tableSize = dirCount * 8;
	// root subtable: "data" (dir 0xF001), "readme.txt" (file 0)
	std::vector<u8> rootSub;
	rootSub.push_back(0x80 | 4);
	for (char c : std::string("data")) rootSub.push_back(u8(c));
	rootSub.push_back(0x01); rootSub.push_back(0xF0);
	rootSub.push_back(10);
	for (char c : std::string("readme.txt")) rootSub.push_back(u8(c));
	rootSub.push_back(0x00);

	std::vector<u8> dataSub;
	for (const char* name : { "a.bin", "b.bin" })
	{
		dataSub.push_back(u8(std::strlen(name)));
		for (const char* p = name; *p; p++) dataSub.push_back(u8(*p));
	}
	dataSub.push_back(0x00);

	put32(tableSize);           put16(0); put16(dirCount);
	put32(tableSize + u32(rootSub.size())); put16(1); put16(0xF000);
	raw.insert(raw.end(), rootSub.begin(), rootSub.end());
	raw.insert(raw.end(), dataSub.begin(), dataSub.end());

	return NitroFs::parse(raw);
}

static void testNitroFs()
{
	const NitroFs tree = buildTree();
	check(tree.directories().size() == 2, "both directories are read");
	check(tree.fileCount() == 3, "three files are named");
	check(tree.findFile("readme.txt") == 0, "a file in the root resolves");
	check(tree.findFile("data/a.bin") == 1, "a file in a subdirectory resolves");
	check(tree.findFile("data/b.bin") == 2, "file ids continue from the directory's first");
	check(tree.findFile("data/missing.bin") == -1, "a missing file is -1, not an exception");
	check(tree.findDirectory("data") == 0xF001, "a directory resolves to its id");
	check(tree.pathOf(0xF001) == "data", "a directory knows its own path");
	check(tree.pathOf(0xF000).empty(), "the root's path is empty");

	// Reserializing and reparsing must describe the same tree.
	const NitroFs again = NitroFs::parse(tree.serialize());
	check(again.fileCount() == 3 && again.findFile("data/b.bin") == 2,
		"a tree survives being written out and read back");

	NitroFs named = tree;
	named.nameFile(0xF001, "c.bin", 3);
	check(named.findFile("data/c.bin") == 3, "a file can be given a name");

	bool threw = false;
	try { named.nameFile(0xF001, "d.bin", 99); } catch (const std::exception&) { threw = true; }
	check(threw, "naming a file that is not the directory's next id is refused");

	NitroFs extended = tree;
	extended.addFile("z_new/reserved", 3);
	extended.addFile("z_new/root.bin", 4);
	extended.addFile("z_new/coop/a.bin", 5);
	extended.addFile("z_new/coop/b.bin", 6);
	check(extended.findFile("z_new/reserved") == 3, "z_new starts at the next FAT id");
	check(extended.findFile("z_new/coop/b.bin") == 6, "new nested directories keep consecutive ids");
	const NitroFs extendedAgain = NitroFs::parse(extended.serialize());
	check(extendedAgain.findFile("z_new/coop/a.bin") == 5,
		"an extended tree survives being written and read");

	threw = false;
	try { extended.addFile("data/late.bin", 7); } catch (const std::exception&) { threw = true; }
	check(threw, "adding to an earlier directory refuses to renumber later files");
}

static void writeFile(const fs::path& path, std::span<const u8> data)
{
	fs::create_directories(path.parent_path());
	std::ofstream out(path, std::ios::binary);
	out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
}

static std::vector<u8> readFile(const fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	return std::vector<u8>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

static void testExtractedNitroFs()
{
	const fs::path root = fs::temp_directory_path() / "ncp_extracted_nitrofs_test";
	std::error_code ignored;
	fs::remove_all(root, ignored);

	const NitroFs tree = buildTree();
	writeFile(root / "fnt.bin", tree.serialize());
	Fat fat;
	for (int i = 0; i < 3; i++)
		fat.add(FatEntry{});
	writeFile(root / "fat.bin", fat.serialize());
	writeFile(root / "data/data/a.bin", std::vector<u8>({ 0x11 }));

	DirRomAccessor rom(root, DirLayout{});
	check(rom.findNitroFile("data/a.bin") == 1, "an extracted FNT resolves a replacement path");
	check(rom.replaceNitroFile("data/a.bin", std::vector<u8>({ 0x22, 0x33 })) == 1,
		"an extracted file keeps its id when replaced");
	check(readFile(root / "data/data/a.bin") == std::vector<u8>({ 0x22, 0x33 }),
		"an extracted replacement writes the loose data file");

	check(rom.addNitroFile("z_new/reserved", {}) == 3, "the extracted reserved file appends to the FAT");
	check(rom.addNitroFile("z_new/coop/new.bin", std::vector<u8>({ 0x44 })) == 4,
		"an extracted z_new file gets the next id");

	const NitroFs afterTree = NitroFs::parse(readFile(root / "fnt.bin"));
	const Fat afterFat = Fat::parse(readFile(root / "fat.bin"));
	check(afterTree.findFile("z_new/reserved") == 3, "the extracted FNT records reserved");
	check(afterTree.findFile("z_new/coop/new.bin") == 4, "the extracted FNT records the new file");
	check(afterFat.size() == 5, "the extracted FAT grows with both files");
	check(readFile(root / "data/z_new/coop/new.bin") == std::vector<u8>({ 0x44 }),
		"the extracted new file is written below data-dir");

	fs::remove_all(root, ignored);
}

// --- the container ---------------------------------------------------------

namespace {

constexpr u32 HEADER_SIZE = 0x200;

// A minimal but structurally honest ROM: header, arm9 (+ nitrocode footer),
// arm9 overlay table, arm7, FAT, and two overlay files.
struct SyntheticRom
{
	std::vector<u8> bytes;
	u32 arm9Offset = HEADER_SIZE;
	u32 arm9Size = 0x100;
	u32 ovtOffset = 0;
	u32 arm7Offset = 0;
	u32 fatOffset = 0;
	u32 file0 = 0;
	u32 file1 = 0;
};

SyntheticRom makeRom(u32 arm9Slack = 0)
{
	SyntheticRom rom;
	std::vector<u8>& b = rom.bytes;

	const u32 arm9End = rom.arm9Offset + rom.arm9Size + NITROCODE_FOOTER_SIZE + arm9Slack;
	rom.ovtOffset = arm9End;
	const u32 ovtSize = 2 * u32(OverlayEntry::SIZE);
	rom.arm7Offset = rom.ovtOffset + ovtSize;
	const u32 arm7Size = 0x80;
	rom.fatOffset = rom.arm7Offset + arm7Size;
	const u32 fatSize = 2 * u32(FatEntry::SIZE);
	rom.file0 = rom.fatOffset + fatSize;
	const u32 file0Size = 0x40;
	rom.file1 = rom.file0 + file0Size;
	const u32 file1Size = 0x40;
	const u32 used = rom.file1 + file1Size;

	b.assign(used, 0);

	std::span<u8> s(b);
	std::memcpy(b.data(), "NCPTEST\0\0\0\0\0", 12);
	std::memcpy(b.data() + 0x0C, "TEST", 4);
	std::memcpy(b.data() + 0x10, "01", 2);
	writeU8(s, 0x14, 0);                       // 128 KiB device
	writeU32(s, 0x20, rom.arm9Offset);
	writeU32(s, 0x24, 0x02000800);
	writeU32(s, 0x28, 0x02000000);
	writeU32(s, 0x2C, rom.arm9Size);
	writeU32(s, 0x30, rom.arm7Offset);
	writeU32(s, 0x34, 0x02380000);
	writeU32(s, 0x38, 0x02380000);
	writeU32(s, 0x3C, arm7Size);
	writeU32(s, 0x48, rom.fatOffset);
	writeU32(s, 0x4C, fatSize);
	writeU32(s, 0x50, rom.ovtOffset);
	writeU32(s, 0x54, ovtSize);
	writeU32(s, 0x80, used);
	writeU32(s, 0x84, HEADER_SIZE);

	// Recognisable contents, so a move can be spotted.
	for (u32 i = 0; i < rom.arm9Size; i++) b[rom.arm9Offset + i] = u8(i);
	writeU32(s, rom.arm9Offset + rom.arm9Size, NITROCODE);
	for (u32 i = 0; i < arm7Size; i++) b[rom.arm7Offset + i] = u8(0x80 + i);
	for (u32 i = 0; i < file0Size; i++) b[rom.file0 + i] = 0xA0;
	for (u32 i = 0; i < file1Size; i++) b[rom.file1 + i] = 0xB0;

	OverlayTable table;
	for (u32 id = 0; id < 2; id++)
	{
		OverlayEntry entry;
		entry.overlayId = id;
		entry.ramAddress = 0x02100000 + id * 0x10000;
		entry.ramSize = 0x40;
		entry.fileId = id;
		table.entries().push_back(entry);
	}
	const std::vector<u8> ovtBytes = table.serialize();
	std::copy(ovtBytes.begin(), ovtBytes.end(), b.begin() + rom.ovtOffset);

	Fat fat;
	fat.add(FatEntry{ rom.file0, rom.file0 + file0Size });
	fat.add(FatEntry{ rom.file1, rom.file1 + file1Size });
	const std::vector<u8> fatBytes = fat.serialize();
	std::copy(fatBytes.begin(), fatBytes.end(), b.begin() + rom.fatOffset);

	Header header;
	header.parse(std::vector<u8>(b.begin(), b.begin() + HEADER_SIZE));
	header.updateChecksum();
	std::copy(header.bytes().begin(), header.bytes().end(), b.begin());

	return rom;
}

// Everything a valid ROM has to keep true of itself.
void checkConsistent(const NdsRom& rom, const std::string& what)
{
	const Header& header = rom.header();
	check(header.storedChecksum() == header.computeChecksum(), what + ": header checksum matches");
	check(header.totalUsedRomSize() <= rom.bytes().size(), what + ": used size is inside the image");
	check(header.arm(true).romOffset == header.headerSize(),
		what + ": arm9 still starts where the header says the header ends");

	struct Extent { u32 start; u32 end; };
	std::vector<Extent> extents;
	for (bool arm9 : { false, true })
	{
		const ArmBinaryInfo arm = header.arm(arm9);
		u32 end = arm.romOffset + arm.size;
		if (arm9 && readU32(rom.bytes(), end) == NITROCODE)
			end += NITROCODE_FOOTER_SIZE;
		extents.push_back({ arm.romOffset, end });

		const RomRegion ovt = header.overlayTable(arm9);
		if (ovt.size != 0)
			extents.push_back({ ovt.romOffset, ovt.end() });
	}
	extents.push_back({ header.fat().romOffset, header.fat().end() });
	if (header.fnt().size != 0)
		extents.push_back({ header.fnt().romOffset, header.fnt().end() });
	for (const FatEntry& entry : rom.fat().entries())
	{
		if (entry.size() != 0)
			extents.push_back({ entry.start, entry.end });
		check(entry.end <= header.totalUsedRomSize(), what + ": every file is inside the used region");
	}

	std::sort(extents.begin(), extents.end(), [](const Extent& a, const Extent& b) { return a.start < b.start; });
	for (std::size_t i = 1; i < extents.size(); i++)
	{
		check(extents[i - 1].end <= extents[i].start,
			what + ": regions do not overlap");
	}
}

} // namespace

static void testHeader()
{
	const SyntheticRom source = makeRom();
	Header header;
	header.parse(std::vector<u8>(source.bytes.begin(), source.bytes.begin() + HEADER_SIZE));

	check(header.gameTitle() == "NCPTEST", "the title is read and its padding trimmed");
	check(header.gameCode() == "TEST", "the game code is read");
	check(header.arm(true).ramAddress == 0x02000000, "the arm9 ram address is read");
	check(header.arm(false).ramAddress == 0x02380000, "the arm7 ram address is read");
	check(header.overlayTable(false).size == 0, "a ROM with no arm7 overlays says so");
	check(header.storedChecksum() == header.computeChecksum(), "the checksum we wrote is the one we compute");
	check(header.deviceCapacityBytes() == 0x20000, "capacity 0 is 128 KiB");

	// Editing one field must leave every other byte alone -- that is the whole
	// reason the header is kept as bytes rather than parsed into a struct.
	const std::vector<u8> before = header.bytes();
	header.setTotalUsedRomSize(0xDEADBEEF);
	const std::vector<u8>& after = header.bytes();
	bool onlyThatField = true;
	for (std::size_t i = 0; i < before.size(); i++)
	{
		const bool inField = (i >= 0x80 && i < 0x84);
		if (!inField && before[i] != after[i])
			onlyThatField = false;
	}
	check(onlyThatField, "setting one field leaves every other byte of the header alone");
	check(header.totalUsedRomSize() == 0xDEADBEEF, "and a value past 0x80000000 survives");

	bool threw = false;
	try { Header().parse(std::vector<u8>(0x100)); } catch (const std::exception&) { threw = true; }
	check(threw, "a header shorter than 0x200 bytes is rejected");
}

static void testRomRoundTrip()
{
	const SyntheticRom source = makeRom();
	NdsRom rom;
	rom.parse(source.bytes);

	check(!rom.dirty(), "a freshly loaded ROM has nothing staged");
	rom.commit(0);
	check(rom.bytes() == source.bytes, "load then save with nothing staged is byte-identical");

	check(rom.readArm(true).size() == source.arm9Size, "arm9 comes back at its recorded size");
	check(rom.overlayTable(true).size() == 2, "both overlay rows are read");
	check(rom.readFile(0).size() == 0x40, "a file comes back at its FAT extent's size");
	check(rom.readFile(0)[0] == 0xA0, "and with its own contents");
}

static void testOverlayInPlace()
{
	const SyntheticRom source = makeRom();
	NdsRom rom;
	rom.parse(source.bytes);

	std::vector<u8> replacement(0x20, 0xCC);
	rom.setFile(0, replacement);
	check(rom.readFile(0)[0] == 0xCC, "a staged write is what a read sees");
	rom.commit(0);

	check(!rom.lastCommitRebuilt(), "a file that shrank did not need a rebuild");
	check(rom.fat().entries()[0].start == source.file0, "it stayed where it was");
	check(rom.fat().entries()[0].size() == 0x20, "and its extent was shortened");
	check(rom.fat().entries()[1].start == source.file1, "the file after it did not move");
	checkConsistent(rom, "after shrinking a file");
}

static void testOverlayRelocated()
{
	const SyntheticRom source = makeRom();
	NdsRom rom;
	rom.parse(source.bytes);

	const u32 usedBefore = rom.header().totalUsedRomSize();
	rom.setFile(0, std::vector<u8>(0x400, 0xDD));
	rom.commit(0);

	check(!rom.lastCommitRebuilt(), "an overlay that outgrew its slot did not force a rebuild");
	check(rom.fat().entries()[0].start >= usedBefore, "it was moved past the end of the used region");
	check(rom.fat().entries()[0].size() == 0x400, "at its new size");
	check(rom.readFile(0) == std::vector<u8>(0x400, 0xDD), "and reads back correctly");
	check(rom.fat().entries()[1].start == source.file1, "the file that fitted did not move");
	check(rom.header().totalUsedRomSize() > usedBefore, "the used size grew to cover it");
	checkConsistent(rom, "after relocating a file");
}

static void testArm9GrowthRebuilds()
{
	const SyntheticRom source = makeRom();
	NdsRom rom;
	rom.parse(source.bytes);

	// No slack in the source ROM, so this cannot be done in place.
	std::vector<u8> bigger(source.arm9Size + 0x200, 0x11);
	rom.setArm(true, bigger);
	rom.commit(0x100);

	check(rom.lastCommitRebuilt(), "growing arm9 past the overlay table forces a rebuild");
	check(rom.header().arm(true).size == bigger.size(), "the new size is recorded");
	check(rom.readArm(true) == bigger, "and the new contents are there");
	check(readU32(rom.bytes(), rom.header().arm(true).romOffset + u32(bigger.size())) == NITROCODE,
		"the nitrocode footer moved with the binary");
	check(rom.readFile(0)[0] == 0xA0 && rom.readFile(1)[0] == 0xB0,
		"every file survived being moved");
	check(rom.overlayTable(true).size() == 2, "so did the overlay table");
	checkConsistent(rom, "after a rebuild");

	// The point of the slack: doing it again with the same sizes must not need
	// a second rebuild, which is what stops every build from rewriting the ROM.
	NdsRom again;
	again.parse(rom.bytes());
	again.setArm(true, bigger);
	again.commit(0x100);
	check(!again.lastCommitRebuilt(), "a second build of the same size fits in the slack");
	check(again.readArm(true) == bigger, "and still has the right contents");
	checkConsistent(again, "after a second in-place arm9 write");
}

static void testTruncatedRom()
{
	const SyntheticRom source = makeRom();

	// Every offset in a ROM comes out of a header some other program wrote, so
	// a file that stops early has to produce a message rather than reading off
	// the end of the buffer.
	for (std::size_t length : { std::size_t(0), std::size_t(100), std::size_t(HEADER_SIZE),
	                            source.bytes.size() / 2 })
	{
		bool threw = false;
		try
		{
			NdsRom rom;
			rom.parse(std::vector<u8>(source.bytes.begin(),
				source.bytes.begin() + std::ptrdiff_t(length)));
		}
		catch (const std::exception&)
		{
			threw = true;
		}
		check(threw, "a ROM truncated to " + std::to_string(length) + " bytes is rejected");
	}
}

static void testFntGrowthRebuilds()
{
	SyntheticRom source = makeRom();
	NdsRom rom;
	rom.parse(source.bytes);
	check(rom.nitroFs().empty(), "the synthetic ROM has no file name table");

	NitroFs tree;
	tree.addFile("z_new/reserved", 2);
	check(rom.addFile({}) == 2, "the reserved file gets the next FAT id");
	tree.addFile("z_new/coop/new.bin", 3);
	check(rom.addFile(std::vector<u8>({ 1, 2, 3, 4 })) == 3, "a new NitroFS file follows it");
	rom.setNitroFs(tree);
	rom.commit(0);

	check(rom.lastCommitRebuilt(), "growing FNT and FAT past their slots rebuilds the ROM");
	check(rom.fat().size() == 4, "both new FAT entries are present");
	check(rom.nitroFs().findFile("z_new/coop/new.bin") == 3, "the new path resolves to its FAT id");
	check(rom.readFile(3) == std::vector<u8>({ 1, 2, 3, 4 }), "the new file data survives the rebuild");
	check(rom.header().fnt().size == rom.nitroFs().serialize().size(), "the header records the grown FNT");
	check(rom.header().fat().size == rom.fat().byteSize(), "the header records the grown FAT");
	checkConsistent(rom, "after adding NitroFS files");

	NdsRom again;
	again.parse(rom.bytes());
	check(again.nitroFs().findFile("z_new/reserved") == 2, "the reserved path reloads");
	check(again.nitroFs().findFile("z_new/coop/new.bin") == 3, "the new path reloads");
	check(again.readFile(3) == std::vector<u8>({ 1, 2, 3, 4 }), "the new data reloads");
}

// --- compression -----------------------------------------------------------

static void testBlz()
{
	// Repetitive enough to compress, long enough to be worth it.
	std::vector<u8> data;
	for (int i = 0; i < 4096; i++)
		data.push_back(u8("the quick brown fox"[i % 19]));

	const std::vector<u8> packed = BLZ::compress(data);
	check(!packed.empty(), "repetitive data compresses");
	check(packed.size() < data.size(), "and gets smaller");
	check(packed.size() % 4 == 0, "the image is aligned so the footer can be read as words");
	check(BLZ::uncompress(packed) == data, "and decompresses back to exactly what went in");

	// Random-looking data must be refused rather than stored larger.
	std::vector<u8> noise(1024);
	u32 state = 0x12345678;
	for (u8& byte : noise)
	{
		state = state * 1103515245 + 12345;
		byte = u8(state >> 16);
	}
	const std::vector<u8> notPacked = BLZ::compress(noise);
	check(notPacked.empty() || notPacked.size() < noise.size(),
		"data that would grow is refused rather than stored");
}

int main()
{
	testEndian();
	testCrc();
	testOverlayTable();
	testFat();
	testNitroFs();
	testExtractedNitroFs();
	testHeader();
	testRomRoundTrip();
	testOverlayInPlace();
	testOverlayRelocated();
	testArm9GrowthRebuilds();
	testTruncatedRom();
	testFntGrowthRebuilds();
	testBlz();

	if (g_failures == 0)
		std::cout << "rom_test: all checks passed\n";
	return g_failures == 0 ? 0 : 1;
}
