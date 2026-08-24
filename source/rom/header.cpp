#include "header.hpp"

#include <fstream>
#include <sstream>

#include "endian.hpp"
#include "../system/diagnostics.hpp"
#include "../system/except.hpp"
#include "../system/log.hpp"
#include "../utils/crc.hpp"

namespace fs = std::filesystem;

namespace ncp::rom {

// Field offsets, GBATEK's cartridge header table. Named rather than inlined so
// that a reader can check them against the documentation one line at a time.
namespace off {
constexpr std::size_t GameTitle      = 0x000;
constexpr std::size_t GameCode       = 0x00C;
constexpr std::size_t MakerCode      = 0x010;
constexpr std::size_t UnitCode       = 0x012;
constexpr std::size_t DeviceCapacity = 0x014;
constexpr std::size_t RomVersion     = 0x01E;
constexpr std::size_t Arm9RomOffset  = 0x020;
constexpr std::size_t Arm7RomOffset  = 0x030;
constexpr std::size_t FntOffset      = 0x040;
constexpr std::size_t FatOffset      = 0x048;
constexpr std::size_t Arm9OvtOffset  = 0x050;
constexpr std::size_t Arm7OvtOffset  = 0x058;
constexpr std::size_t BannerOffset   = 0x068;
constexpr std::size_t Arm9HookAddr   = 0x070;
constexpr std::size_t Arm7HookAddr   = 0x074;
constexpr std::size_t TotalUsedSize  = 0x080;
constexpr std::size_t HeaderSize     = 0x084;
constexpr std::size_t HeaderChecksum = 0x15E;
} // namespace off

// The four ARM binary fields are one 16-byte block per processor, so every
// accessor below is the same arithmetic with a different base.
static constexpr std::size_t armBase(bool arm9)
{
	return arm9 ? off::Arm9RomOffset : off::Arm7RomOffset;
}

static constexpr std::size_t ovtBase(bool arm9)
{
	return arm9 ? off::Arm9OvtOffset : off::Arm7OvtOffset;
}

void Header::parse(std::vector<u8> bytes)
{
	if (bytes.size() < SIZE)
	{
		std::ostringstream oss;
		oss << "Invalid ROM header: expected a minimum of " << SIZE
		    << " bytes, got " << bytes.size() << " bytes.";
		throw ncp::exception(oss.str());
	}
	m_bytes = std::move(bytes);
}

void Header::load(const fs::path& path)
{
	ncp::ScopedContext ctx(ncp::Diag::RomHeaderLoad, "Could not load the ROM header.");

	Log::info("Loading header file...");

	if (!fs::exists(path))
		throw ncp::file_error(path, ncp::file_error::find);

	std::ifstream file(path, std::ios::binary);
	if (!file.is_open())
		throw ncp::file_error(path, ncp::file_error::read);

	const uintmax_t fileSize = fs::file_size(path);
	std::vector<u8> bytes;
	bytes.resize(std::size_t(fileSize));
	file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
	if (!file)
		throw ncp::file_error(path, ncp::file_error::read);
	file.close();

	try
	{
		parse(std::move(bytes));
	}
	catch (const std::exception& e)
	{
		std::ostringstream oss;
		oss << "Invalid ROM header file: " << OSTR(path.string()) << OREASONNL << e.what();
		throw ncp::exception(oss.str());
	}
}

std::string Header::text(std::size_t offset, std::size_t length) const
{
	requireRange(read(), offset, length);
	std::string out(reinterpret_cast<const char*>(m_bytes.data()) + offset, length);
	// The fields are space- or zero-padded depending on who built the ROM.
	while (!out.empty() && (out.back() == '\0' || out.back() == ' '))
		out.pop_back();
	return out;
}

std::string Header::gameTitle() const { return text(off::GameTitle, 12); }
std::string Header::gameCode() const  { return text(off::GameCode, 4); }
std::string Header::makerCode() const { return text(off::MakerCode, 2); }

u8 Header::unitCode() const   { return readU8(read(), off::UnitCode); }
u8 Header::romVersion() const { return readU8(read(), off::RomVersion); }

u8 Header::deviceCapacity() const { return readU8(read(), off::DeviceCapacity); }
void Header::setDeviceCapacity(u8 value) { writeU8(write(), off::DeviceCapacity, value); }

u32 Header::deviceCapacityBytes() const
{
	// GBATEK: chip size is 128 KiB shifted left by the stored value. Clamped
	// because a corrupt byte would otherwise shift a u32 by more than 31.
	const u8 shift = deviceCapacity();
	return shift >= 15 ? 0xFFFFFFFFu : (0x20000u << shift);
}

ArmBinaryInfo Header::arm(bool arm9) const
{
	const std::size_t base = armBase(arm9);
	ArmBinaryInfo info;
	info.romOffset    = readU32(read(), base + 0x0);
	info.entryAddress = readU32(read(), base + 0x4);
	info.ramAddress   = readU32(read(), base + 0x8);
	info.size         = readU32(read(), base + 0xC);
	return info;
}

void Header::setArmRomOffset(bool arm9, u32 value) { writeU32(write(), armBase(arm9) + 0x0, value); }
void Header::setArmSize(bool arm9, u32 value)      { writeU32(write(), armBase(arm9) + 0xC, value); }

u32 Header::autoLoadListHookAddress(bool arm9) const
{
	return readU32(read(), arm9 ? off::Arm9HookAddr : off::Arm7HookAddr);
}

RomRegion Header::fnt() const { return { readU32(read(), off::FntOffset), readU32(read(), off::FntOffset + 4) }; }
RomRegion Header::fat() const { return { readU32(read(), off::FatOffset), readU32(read(), off::FatOffset + 4) }; }

void Header::setFnt(RomRegion region)
{
	writeU32(write(), off::FntOffset, region.romOffset);
	writeU32(write(), off::FntOffset + 4, region.size);
}

void Header::setFat(RomRegion region)
{
	writeU32(write(), off::FatOffset, region.romOffset);
	writeU32(write(), off::FatOffset + 4, region.size);
}

RomRegion Header::overlayTable(bool arm9) const
{
	const std::size_t base = ovtBase(arm9);
	return { readU32(read(), base), readU32(read(), base + 4) };
}

void Header::setOverlayTable(bool arm9, RomRegion region)
{
	const std::size_t base = ovtBase(arm9);
	// A ROM with no overlays stores offset 0 as well as size 0, and a
	// zero-length table pointed at a real offset confuses some tools, so keep
	// the pair consistent.
	writeU32(write(), base, region.size == 0 ? 0 : region.romOffset);
	writeU32(write(), base + 4, region.size);
}

u32 Header::bannerOffset() const { return readU32(read(), off::BannerOffset); }
void Header::setBannerOffset(u32 value) { writeU32(write(), off::BannerOffset, value); }

u32 Header::totalUsedRomSize() const { return readU32(read(), off::TotalUsedSize); }
void Header::setTotalUsedRomSize(u32 value) { writeU32(write(), off::TotalUsedSize, value); }

u32 Header::headerSize() const { return readU32(read(), off::HeaderSize); }

u16 Header::storedChecksum() const { return readU16(read(), off::HeaderChecksum); }

u16 Header::computeChecksum() const
{
	return Crc::modbus16(std::span<const u8>(m_bytes).subspan(0, CHECKSUM_COVERAGE));
}

void Header::updateChecksum()
{
	writeU16(write(), off::HeaderChecksum, computeChecksum());
}

bool Header::isDsi() const
{
	// GBATEK: bit 1 of the unit code marks a title the DSi will run, and those
	// are the ROMs carrying the extended header.
	return (unitCode() & 0x02) != 0;
}

} // namespace ncp::rom
