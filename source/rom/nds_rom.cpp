#include "nds_rom.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "layout.hpp"
#include "../utils/crc.hpp"
#include "../utils/endian.hpp"
#include "../system/except.hpp"
#include "../system/log.hpp"

namespace fs = std::filesystem;

namespace ncp::rom {

void NdsRom::load(const fs::path& path)
{
	if (!fs::exists(path))
		throw ncp::file_error(path, ncp::file_error::find);

	std::ifstream file(path, std::ios::binary);
	if (!file.is_open())
		throw ncp::file_error(path, ncp::file_error::read);

	std::vector<u8> bytes;
	bytes.resize(std::size_t(fs::file_size(path)));
	if (!bytes.empty())
	{
		file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
		if (!file)
			throw ncp::file_error(path, ncp::file_error::read);
	}
	file.close();

	try
	{
		parse(std::move(bytes));
	}
	catch (const std::exception& e)
	{
		std::ostringstream oss;
		oss << "Not a usable ROM: " << OSTR(path.string()) << OREASONNL << e.what();
		throw ncp::exception(oss.str());
	}
}

// A region of the image, refusing to point outside it.
//
// std::span::subspan is undefined behavior when the offset is past the end,
// and every one of these offsets comes out of a header some other program
// wrote. A truncated download has to produce a message naming the region that
// does not fit, not a crash.
std::span<const u8> NdsRom::region(const RomRegion& region, const char* what) const
{
	if (region.romOffset > m_bytes.size() || region.size > m_bytes.size() - region.romOffset)
	{
		std::ostringstream oss;
		oss << "The header puts the " << what << " at 0x" << std::hex << std::uppercase
		    << region.romOffset << " with a size of 0x" << region.size << std::nouppercase
		    << std::dec << ", which is past the end of a " << m_bytes.size() << "-byte file.";
		throw ncp::exception(oss.str());
	}
	return std::span<const u8>(m_bytes).subspan(region.romOffset, region.size);
}

void NdsRom::parse(std::vector<u8> bytes)
{
	m_bytes = std::move(bytes);
	m_header.parse(std::vector<u8>(m_bytes.begin(),
		m_bytes.begin() + std::ptrdiff_t(std::min(m_bytes.size(), Header::SIZE))));

	const RomRegion fatRegion = m_header.fat();
	if (fatRegion.size != 0)
		m_fat = Fat::parse(region(fatRegion, "file allocation table"));

	const RomRegion fntRegion = m_header.fnt();
	if (fntRegion.size != 0)
		m_nitroFs = NitroFs::parse(region(fntRegion, "file name table"));

	for (bool arm9 : { false, true })
	{
		const RomRegion ovtRegion = m_header.overlayTable(arm9);
		if (ovtRegion.size != 0)
		{
			m_ovt[arm9 ? 1 : 0] = OverlayTable::parse(
				region(ovtRegion, arm9 ? "ARM9 overlay table" : "ARM7 overlay table"));
		}
	}

	// Checked here rather than left to whoever reads them first, so that the
	// message names the ROM rather than whatever phase happened to touch it.
	for (bool arm9 : { false, true })
	{
		const ArmBinaryInfo arm = m_header.arm(arm9);
		(void)region(RomRegion{ arm.romOffset, arm.size }, arm9 ? "ARM9 binary" : "ARM7 binary");
	}
}

void NdsRom::save(const fs::path& path) const
{
	// Written beside the destination rather than to a system temporary
	// directory, so that the rename stays on one filesystem and therefore stays
	// atomic. A cross-device rename is a copy, and a copy can be interrupted.
	fs::path temporary = path;
	temporary += ".ncp-tmp";

	if (path.has_parent_path())
		fs::create_directories(path.parent_path());

	{
		std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
		if (!file.is_open())
			throw ncp::file_error(temporary, ncp::file_error::write);
		file.write(reinterpret_cast<const char*>(m_bytes.data()), std::streamsize(m_bytes.size()));
		if (!file)
			throw ncp::file_error(temporary, ncp::file_error::write);
	}

	std::error_code error;
	fs::rename(temporary, path, error);
	if (error)
	{
		fs::remove(temporary, error);
		throw ncp::file_error(path, ncp::file_error::write);
	}
}

std::vector<u8> NdsRom::slice(u32 offset, u32 size) const
{
	le::requireRange(m_bytes, offset, size);
	return std::vector<u8>(m_bytes.begin() + std::ptrdiff_t(offset),
	                       m_bytes.begin() + std::ptrdiff_t(offset) + std::ptrdiff_t(size));
}

bool NdsRom::hasNitrocodeFooter() const
{
	const ArmBinaryInfo info = m_header.arm(true);
	const u32 end = info.romOffset + info.size;
	if (std::size_t(end) + 4 > m_bytes.size())
		return false;
	return le::readU32(m_bytes, end) == NITROCODE;
}

u32 NdsRom::bannerSize() const
{
	const u32 offset = m_header.bannerOffset();
	if (offset == 0 || std::size_t(offset) + 2 > m_bytes.size())
		return 0;

	// GBATEK's icon/title versions. An unknown one falls back to the smallest
	// documented size, which is what every version has in common.
	switch (le::readU16(m_bytes, offset))
	{
	case 0x0001: return 0x0840;
	case 0x0002: return 0x0940;
	case 0x0003: return 0x0A40;
	case 0x0103: return 0x23C0;
	default:     return 0x0840;
	}
}

std::vector<u8> NdsRom::readArm(bool arm9) const
{
	const int slot = arm9 ? 1 : 0;
	if (m_pendingArm[slot].has_value())
		return *m_pendingArm[slot];

	const ArmBinaryInfo info = m_header.arm(arm9);
	return slice(info.romOffset, info.size);
}

bool NdsRom::hasFile(u32 fileId) const
{
	return fileId < m_fat.size();
}

u32 NdsRom::fileSize(u32 fileId) const
{
	const auto pending = m_pendingFiles.find(fileId);
	if (pending != m_pendingFiles.end())
		return u32(pending->second.size());

	return hasFile(fileId) ? m_fat.entries()[fileId].size() : 0;
}

std::vector<u8> NdsRom::readFile(u32 fileId) const
{
	const auto pending = m_pendingFiles.find(fileId);
	if (pending != m_pendingFiles.end())
		return pending->second;

	if (!hasFile(fileId))
	{
		std::ostringstream oss;
		oss << "No file with id " << fileId << " in this ROM; it has "
		    << m_fat.size() << " file(s).";
		throw ncp::exception(oss.str());
	}

	const FatEntry& entry = m_fat.entries()[fileId];
	return slice(entry.start, entry.size());
}

void NdsRom::setArm(bool arm9, std::vector<u8> data)
{
	m_pendingArm[arm9 ? 1 : 0] = std::move(data);
}

void NdsRom::setOverlayTable(bool arm9, OverlayTable table)
{
	m_pendingOvt[arm9 ? 1 : 0] = std::move(table);
}

std::vector<u8> NdsRom::readBanner() const
{
	if (m_pendingBanner.has_value())
		return *m_pendingBanner;

	const u32 size = bannerSize();
	return size != 0 ? slice(m_header.bannerOffset(), size) : std::vector<u8>();
}

void NdsRom::setBanner(std::vector<u8> data)
{
	const u32 size = bannerSize();
	if (size == 0)
		throw ncp::exception("This ROM has no icon/title banner to replace.");
	if (data.size() != size)
	{
		std::ostringstream oss;
		oss << "Cannot replace the banner with " << data.size() << " bytes: the ROM's is "
		    << size << "." OREASONNL
		    << "A banner's length is fixed by the version it declares, and growing the region "
		       "would mean laying the whole container out again.";
		throw ncp::exception(oss.str());
	}
	m_pendingBanner = std::move(data);
}

void NdsRom::setFile(u32 fileId, std::vector<u8> data)
{
	if (!hasFile(fileId))
	{
		std::ostringstream oss;
		oss << "Cannot write file id " << fileId << ": this ROM has "
		    << m_fat.size() << " file(s).";
		throw ncp::exception(oss.str());
	}
	m_pendingFiles[fileId] = std::move(data);
}

void NdsRom::setNitroFs(const NitroFs& tree)
{
	m_nitroFs = tree;
	m_pendingFnt = tree.serialize();
}

u32 NdsRom::addFile(std::vector<u8> data)
{
	const u32 fileId = m_fat.add(FatEntry{ 0, 0 });
	m_pendingFiles[fileId] = std::move(data);
	return fileId;
}

bool NdsRom::dirty() const
{
	return m_pendingArm[0].has_value() || m_pendingArm[1].has_value()
	    || m_pendingOvt[0].has_value() || m_pendingOvt[1].has_value()
	    || !m_pendingFiles.empty() || m_pendingFnt.has_value()
	    || m_pendingBanner.has_value();
}

void NdsRom::writeRegion(u32 offset, std::span<const u8> data)
{
	if (std::size_t(offset) + data.size() > m_bytes.size())
		m_bytes.resize(std::size_t(offset) + data.size(), 0xFF);
	std::copy(data.begin(), data.end(), m_bytes.begin() + std::ptrdiff_t(offset));
}

std::vector<u32> NdsRom::occupiedStarts() const
{
	std::vector<u32> starts;
	starts.reserve(m_fat.size() + 8);

	for (bool arm9 : { false, true })
	{
		const ArmBinaryInfo info = m_header.arm(arm9);
		if (info.size != 0)
			starts.push_back(info.romOffset);

		const RomRegion ovt = m_header.overlayTable(arm9);
		if (ovt.size != 0)
			starts.push_back(ovt.romOffset);
	}

	for (const RomRegion region : { m_header.fnt(), m_header.fat() })
	{
		if (region.size != 0)
			starts.push_back(region.romOffset);
	}

	if (m_header.bannerOffset() != 0)
		starts.push_back(m_header.bannerOffset());

	for (const FatEntry& entry : m_fat.entries())
	{
		if (entry.size() != 0)
			starts.push_back(entry.start);
	}

	std::sort(starts.begin(), starts.end());
	starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
	return starts;
}

u32 NdsRom::roomAfter(u32 start, const std::vector<u32>& starts) const
{
	const auto next = std::upper_bound(starts.begin(), starts.end(), start);
	const u32 limit = (next == starts.end()) ? m_header.totalUsedRomSize() : *next;
	return limit > start ? limit - start : 0;
}

void NdsRom::commit(u32 arm9Slack)
{
	m_lastCommitRebuilt = false;
	if (!dirty())
		return;

	const std::vector<u32> starts = occupiedStarts();

	// Does everything still fit where it is? The ARM binaries and the two
	// overlay tables are named by fixed header fields, so growing one past its
	// neighbor means moving the neighbor, which means moving everything:
	// there is no way to relocate ARM9 on its own, because the secure area is
	// encrypted against its being at the fixed offset the header size names.
	bool needsRebuild = false;

	const u32 footer = hasNitrocodeFooter() ? NITROCODE_FOOTER_SIZE : 0;

	for (bool arm9 : { false, true })
	{
		const int slot = arm9 ? 1 : 0;
		if (m_pendingArm[slot].has_value())
		{
			const ArmBinaryInfo info = m_header.arm(arm9);
			const u32 needed = u32(m_pendingArm[slot]->size()) + (arm9 ? footer : 0);
			if (needed > roomAfter(info.romOffset, starts))
				needsRebuild = true;
		}
		if (m_pendingOvt[slot].has_value())
		{
			const RomRegion ovt = m_header.overlayTable(arm9);
			const u32 needed = u32(m_pendingOvt[slot]->byteSize());
			// A table that did not exist before has nowhere to go but a rebuild.
			if (ovt.size == 0 ? needed != 0 : needed > roomAfter(ovt.romOffset, starts))
				needsRebuild = true;
		}
	}

	if (m_pendingFnt.has_value())
	{
		const RomRegion fnt = m_header.fnt();
		if (fnt.size == 0 || u32(m_pendingFnt->size()) > roomAfter(fnt.romOffset, starts))
			needsRebuild = true;
	}

	// addFile() grows the FAT itself, not just one of the extents it names.
	// Treat it like FNT growth: writing a longer table over the next region is
	// corruption, so rebuild unless the old table's gap can hold it.
	const RomRegion fatTableRegion = m_header.fat();
	if (fatTableRegion.size == 0 ? !m_fat.empty()
		: u32(m_fat.byteSize()) > roomAfter(fatTableRegion.romOffset, starts))
		needsRebuild = true;

	if (needsRebuild)
	{
		rebuildLayout(arm9Slack);
		return;
	}

	// In place. Each region keeps its offset; only its recorded size changes.
	for (bool arm9 : { false, true })
	{
		const int slot = arm9 ? 1 : 0;
		if (m_pendingArm[slot].has_value())
		{
			const std::vector<u8>& data = *m_pendingArm[slot];
			const ArmBinaryInfo info = m_header.arm(arm9);
			writeRegion(info.romOffset, data);
			m_header.setArmSize(arm9, u32(data.size()));
			if (arm9 && footer != 0)
			{
				// The footer follows the binary, so it moves when the binary's
				// length changes. Its own contents are unchanged.
				const std::vector<u8> tail = slice(info.romOffset + info.size, footer);
				writeRegion(info.romOffset + u32(data.size()), tail);
			}
		}
		if (m_pendingOvt[slot].has_value())
		{
			const std::vector<u8> data = m_pendingOvt[slot]->serialize();
			const RomRegion ovt = m_header.overlayTable(arm9);
			writeRegion(ovt.romOffset, data);
			m_header.setOverlayTable(arm9, RomRegion{ ovt.romOffset, u32(data.size()) });
			m_ovt[slot] = *m_pendingOvt[slot];
		}
	}

	if (m_pendingFnt.has_value())
	{
		const RomRegion fnt = m_header.fnt();
		writeRegion(fnt.romOffset, *m_pendingFnt);
		m_header.setFnt(RomRegion{ fnt.romOffset, u32(m_pendingFnt->size()) });
	}

	// The banner is the one region that can never need moving: setBanner
	// refuses a length change, so the replacement fits exactly where the old
	// one was and the header keeps pointing at it.
	if (m_pendingBanner.has_value())
		writeRegion(m_header.bannerOffset(), *m_pendingBanner);

	// Files: back into their own extent when they still fit, appended to the
	// end of the used ROM when they do not. The abandoned extent becomes dead
	// space, which `ncpatcher rom pack` reclaims.
	u32 used = m_header.totalUsedRomSize();
	for (const auto& [fileId, data] : m_pendingFiles)
	{
		FatEntry& entry = m_fat.entries()[fileId];
		const u32 size = u32(data.size());
		if (entry.size() != 0 && size <= roomAfter(entry.start, starts))
		{
			writeRegion(entry.start, data);
			entry.end = entry.start + size;
		}
		else
		{
			const u32 offset = alignUp(used, 4);
			writeRegion(offset, data);
			entry.start = offset;
			entry.end = offset + size;
			used = entry.end;
		}
	}

	if (!m_pendingFiles.empty())
	{
		const std::vector<u8> fatBytes = m_fat.serialize();
		const RomRegion fatRegion = m_header.fat();
		writeRegion(fatRegion.romOffset, fatBytes);
		m_header.setFat(RomRegion{ fatRegion.romOffset, u32(fatBytes.size()) });
	}

	m_header.setTotalUsedRomSize(std::max(m_header.totalUsedRomSize(), used));
	if (m_header.totalUsedRomSize() > m_header.deviceCapacityBytes())
	{
		u8 shift = m_header.deviceCapacity();
		while (shift < 15 && (0x20000u << shift) < m_header.totalUsedRomSize())
			shift++;
		m_header.setDeviceCapacity(shift);
	}

	m_header.updateChecksum();
	writeRegion(0, m_header.bytes());

	m_pendingArm[0].reset();
	m_pendingArm[1].reset();
	m_pendingOvt[0].reset();
	m_pendingOvt[1].reset();
	m_pendingFiles.clear();
	m_pendingFnt.reset();
}

namespace {

// [090h]/[092h] count in 512 KiB units, so the TWL area starts on one.
constexpr u32 TWL_ALIGN = 0x80000;

// Dedicated ARM9i/ARM7i load areas (GBATEK's usual addresses). The launcher
// copies the stubs there before the game starts; nothing runs or reads them.
constexpr u32 ARM9I_RAM = 0x02400000;
constexpr u32 ARM7I_RAM = 0x02E80000;
constexpr u32 TWL_STUB_SIZE = 0x400;

// [1B8h] ARM7 SCFG_EXT7, of which the launcher takes bits 0, 1, 2, 10, 18 and
// 31. Bit 31 keeps SCFG/MBK access so the code can use DSi hardware; the rest
// stays off, which keeps the NITRO DMA/sound circuits the NTR SDK expects.
constexpr u32 SCFG_EXT7 = 1u << 31;

// [180h..1AFh] NWRAM. All of WRAM-A/B/C goes to the ARM9 as one 768 KiB window
// at 0x03000000 and the ARM7 gets none, so it sees what it would in DS mode.
// MBK9 = 0 leaves the slot registers writable from ARM9. [1AFh] WRAMCNT = 3
// gives the old 32 KiB shared WRAM entirely to the ARM7, as in DS mode.
constexpr u32 NWRAM_MBK1_5[5] = { 0x8C888480, 0x8C888480, 0x9C989490, 0x8C888480, 0x9C989490 };
constexpr u32 NWRAM_MBK6_8_ARM9[3] = {
	(0u << 4)  | (3u << 12) | (4u << 20),    // WRAM-A 0x03000000-0x0303FFFF
	(8u << 3)  | (3u << 12) | (16u << 19),   // WRAM-B 0x03040000-0x0307FFFF
	(16u << 3) | (3u << 12) | (24u << 19),   // WRAM-C 0x03080000-0x030BFFFF
};

std::vector<u8> twlStub(const char* name)
{
	std::string text = std::string("NCPatcher ") + name + " stub: not executed";
	std::vector<u8> out(TWL_STUB_SIZE, 0);
	std::copy(text.begin(), text.end(), out.begin());
	return out;
}

} // namespace

// Whether the ROM is the output of makeDsiEnhanced, as opposed to a game that
// has real DSi binaries of its own.
bool NdsRom::hasOwnTwlStubs() const
{
	if (m_bytes.size() < Header::DSI_SIZE)
		return false;
	const std::span<const u8> b(m_bytes);
	for (std::size_t entry : { std::size_t(0x1C0), std::size_t(0x1D0) })
	{
		const u32 offset = le::readU32(b, entry);
		const u32 size = le::readU32(b, entry + 0xC);
		const std::vector<u8> stub = twlStub(entry == 0x1C0 ? "ARM9i" : "ARM7i");
		if (size != stub.size() || offset > m_bytes.size() || m_bytes.size() - offset < size)
			return false;
		if (!std::equal(stub.begin(), stub.end(), m_bytes.begin() + offset))
			return false;
	}
	return true;
}

void NdsRom::makeDsiEnhanced()
{
	if (m_bytes.size() < 0x4000)
		throw ncp::exception("The ROM is too small to hold a DSi header.");

	const u8 unit = m_header.unitCode();
	if (unit != 0x00 && unit != 0x02)
	{
		std::ostringstream oss;
		oss << "Cannot make the ROM DSi-enhanced: unit code 0x" << std::hex << unsigned(unit)
		    << " is neither NDS nor NDS+DSi.";
		throw ncp::exception(oss.str());
	}
	if (unit == 0x02 && !hasOwnTwlStubs())
		throw ncp::exception("Cannot make the ROM DSi-enhanced: it already is, and its ARM9i/ARM7i binaries would be replaced by stubs.");
	if (m_header.arm(true).romOffset < 0x4000)
		throw ncp::exception("Cannot make the ROM DSi-enhanced: ARM9 starts inside the 16 KiB DSi header area.");
	for (std::size_t i = 0x1000; i < 0x4000; i++)
	{
		if (m_bytes[i] != 0)
			throw ncp::exception("Cannot make the ROM DSi-enhanced: 0x1000-0x3FFF is not empty, so the DSi header area is in use.");
	}

	// Always from the NTR total, which the DSi fields never touch, so that
	// running this again on its own output lands on the same layout.
	const u32 twlStart = (m_header.totalUsedRomSize() + TWL_ALIGN - 1) / TWL_ALIGN;
	const u32 arm9iRom = twlStart * TWL_ALIGN;
	const u32 arm7iRom = arm9iRom + TWL_STUB_SIZE;

	std::vector<u8> stubs = twlStub("ARM9i");
	const std::vector<u8> arm7iStub = twlStub("ARM7i");
	stubs.insert(stubs.end(), arm7iStub.begin(), arm7iStub.end());

	std::vector<u8> hdr(m_bytes.begin(), m_bytes.begin() + Header::DSI_SIZE);
	std::span<u8> h(hdr);

	le::writeU8(h, 0x012, 0x02);
	le::writeU8(h, 0x01C, 0x01);  // has a TWL-exclusive region; loaders ignore ARM9i/ARM7i without it
	le::writeU16(h, 0x090, u16(twlStart));
	le::writeU16(h, 0x092, u16(twlStart));

	std::fill(hdr.begin() + 0x180, hdr.begin() + 0x1B0, 0);
	for (int i = 0; i < 5; i++)
		le::writeU32(h, 0x180 + 4 * std::size_t(i), NWRAM_MBK1_5[i]);
	for (int i = 0; i < 3; i++)
		le::writeU32(h, 0x194 + 4 * std::size_t(i), NWRAM_MBK6_8_ARM9[i]);
	le::writeU8(h, 0x1AF, 0x03);

	le::writeU32(h, 0x1B0, 0xFFFFFFFF);  // region free
	le::writeU32(h, 0x1B4, 0);           // no access to SD, NAND or AES keys
	le::writeU32(h, 0x1B8, SCFG_EXT7);
	le::writeU8(h, 0x1BF, 0x00);         // touchscreen/sound controller stays in NDS mode

	le::writeU32(h, 0x1C0, arm9iRom);
	le::writeU32(h, 0x1C4, 0);
	le::writeU32(h, 0x1C8, ARM9I_RAM);
	le::writeU32(h, 0x1CC, TWL_STUB_SIZE);
	le::writeU32(h, 0x1D0, arm7iRom);
	le::writeU32(h, 0x1D4, 0);
	le::writeU32(h, 0x1D8, ARM7I_RAM);
	le::writeU32(h, 0x1DC, TWL_STUB_SIZE);

	le::writeU32(h, 0x208, bannerSize());
	le::writeU32(h, 0x210, arm9iRom + u32(stubs.size()));  // total used, TWL area included

	// Title ID: the game code reversed, as a cartridge DSi title.
	for (std::size_t i = 0; i < 4; i++)
		hdr[0x230 + i] = hdr[0x00F - i];
	hdr[0x234] = 0x00; hdr[0x235] = 0x00; hdr[0x236] = 0x03; hdr[0x237] = 0x00;
	std::fill(hdr.begin() + 0x2F0, hdr.begin() + 0x300, 0x80);  // no age restrictions

	le::writeU16(h, 0x15E, Crc::modbus16(std::span<const u8>(hdr).subspan(0, Header::CHECKSUM_COVERAGE)));

	std::copy(hdr.begin(), hdr.end(), m_bytes.begin());
	writeRegion(arm9iRom, stubs);
	m_header.parse(std::vector<u8>(m_bytes.begin(), m_bytes.begin() + std::ptrdiff_t(Header::SIZE)));
}

// Lays the whole ROM out again, in the order a cartridge is normally built.
//
// This is what happens when the ARM9 binary outgrows the gap before the overlay
// table: nothing after it can stay where it is. It is also the expensive case,
// because every file offset in the ROM changes, so a slack allowance is left
// after ARM9. The patcher rebuilds ARM9 from the pristine backup on every
// build, so its patched size is a function of the code and not of how many
// times the project has been built, which is what makes one rebuild enough
// rather than one per build.
void NdsRom::rebuildLayout(u32 arm9Slack)
{
	m_lastCommitRebuilt = true;

	// Gather every region's contents before anything moves.
	std::vector<u8> arm9Data = readArm(true);
	std::vector<u8> arm7Data = readArm(false);
	std::vector<u8> footerData;
	if (hasNitrocodeFooter())
	{
		const ArmBinaryInfo info = m_header.arm(true);
		footerData = slice(info.romOffset + info.size, NITROCODE_FOOTER_SIZE);
	}

	std::vector<u8> ovtData[2];
	for (bool arm9 : { false, true })
	{
		const int slot = arm9 ? 1 : 0;
		if (m_pendingOvt[slot].has_value())
			ovtData[slot] = m_pendingOvt[slot]->serialize();
		else if (m_header.overlayTable(arm9).size != 0)
			ovtData[slot] = m_ovt[slot].serialize();
	}

	const RomRegion fntRegion = m_header.fnt();
	const std::vector<u8> fntData = m_pendingFnt.has_value()
		? *m_pendingFnt
		: (fntRegion.size != 0 ? slice(fntRegion.romOffset, fntRegion.size) : std::vector<u8>());

	const std::vector<u8> bannerData = readBanner();

	std::vector<std::vector<u8>> files(m_fat.size());
	for (std::size_t i = 0; i < m_fat.size(); i++)
	{
		const auto pending = m_pendingFiles.find(u32(i));
		files[i] = (pending != m_pendingFiles.end()) ? pending->second : slice(m_fat.entries()[i].start, m_fat.entries()[i].size());
	}

	// Place them. The header size is where ARM9 has to start: the secure area
	// sits inside ARM9's first bytes and the BIOS expects it at that offset.
	//
	// The new image starts as padding rather than as a copy of the old one. A
	// rebuild moves every file, so nothing outside a placed region is worth
	// keeping, including whatever a dumper left past the end of the old
	// image, which would otherwise sit in the middle of the new one.
	std::vector<u8> out(m_bytes.size(), 0xFF);
	std::copy(m_bytes.begin(), m_bytes.begin() + std::ptrdiff_t(m_header.headerSize()), out.begin());

	auto place = [&out](u32 offset, std::span<const u8> data) {
		if (std::size_t(offset) + data.size() > out.size())
			out.resize(std::size_t(offset) + data.size(), 0xFF);
		std::copy(data.begin(), data.end(), out.begin() + std::ptrdiff_t(offset));
	};

	u32 cursor = m_header.headerSize();

	const u32 arm9Offset = cursor;
	place(arm9Offset, arm9Data);
	cursor = arm9Offset + u32(arm9Data.size());
	if (!footerData.empty())
	{
		place(cursor, footerData);
		cursor += u32(footerData.size());
	}
	cursor = alignUp(cursor + arm9Slack, 4);

	u32 ovtOffset[2] = { 0, 0 };
	for (bool arm9 : { true, false })
	{
		const int slot = arm9 ? 1 : 0;
		if (ovtData[slot].empty())
			continue;
		ovtOffset[slot] = cursor;
		place(cursor, ovtData[slot]);
		cursor = alignUp(cursor + u32(ovtData[slot].size()), 4);
	}

	const u32 arm7Offset = cursor;
	place(arm7Offset, arm7Data);
	cursor = alignUp(cursor + u32(arm7Data.size()), 4);

	const u32 fntOffset = fntData.empty() ? 0 : cursor;
	if (!fntData.empty())
	{
		place(cursor, fntData);
		cursor = alignUp(cursor + u32(fntData.size()), 4);
	}

	const u32 fatOffset = cursor;
	const u32 fatLength = u32(m_fat.byteSize());
	cursor = alignUp(cursor + fatLength, 4);  // filled in below, once the entries are final

	const u32 bannerOffset = bannerData.empty() ? 0 : cursor;
	if (!bannerData.empty())
	{
		place(cursor, bannerData);
		cursor = alignUp(cursor + u32(bannerData.size()), 4);
	}

	for (std::size_t i = 0; i < files.size(); i++)
	{
		FatEntry& entry = m_fat.entries()[i];
		if (files[i].empty())
		{
			// An empty file still needs an extent the game can read as empty.
			entry.start = cursor;
			entry.end = cursor;
			continue;
		}
		place(cursor, files[i]);
		entry.start = cursor;
		entry.end = cursor + u32(files[i].size());
		cursor = alignUp(entry.end, 4);
	}

	place(fatOffset, m_fat.serialize());

	// Update the header last, from the offsets that were actually used.
	m_header.setArmRomOffset(true, arm9Offset);
	m_header.setArmSize(true, u32(arm9Data.size()));
	m_header.setArmRomOffset(false, arm7Offset);
	m_header.setArmSize(false, u32(arm7Data.size()));
	for (bool arm9 : { false, true })
	{
		const int slot = arm9 ? 1 : 0;
		m_header.setOverlayTable(arm9, RomRegion{ ovtOffset[slot], u32(ovtData[slot].size()) });
	}
	m_header.setFnt(RomRegion{ fntOffset, u32(fntData.size()) });
	m_header.setFat(RomRegion{ fatOffset, fatLength });
	if (!bannerData.empty())
		m_header.setBannerOffset(bannerOffset);
	m_header.setTotalUsedRomSize(cursor);

	if (cursor > m_header.deviceCapacityBytes())
	{
		u8 shift = m_header.deviceCapacity();
		while (shift < 15 && (0x20000u << shift) < cursor)
			shift++;
		m_header.setDeviceCapacity(shift);
	}

	m_header.updateChecksum();
	std::copy(m_header.bytes().begin(), m_header.bytes().end(), out.begin());

	m_bytes = std::move(out);

	for (bool arm9 : { false, true })
	{
		const int slot = arm9 ? 1 : 0;
		if (m_pendingOvt[slot].has_value())
			m_ovt[slot] = *m_pendingOvt[slot];
	}

	m_pendingArm[0].reset();
	m_pendingArm[1].reset();
	m_pendingOvt[0].reset();
	m_pendingOvt[1].reset();
	m_pendingFiles.clear();
	m_pendingFnt.reset();
}

} // namespace ncp::rom
