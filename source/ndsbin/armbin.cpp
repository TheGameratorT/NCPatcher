#include "armbin.hpp"

#include <iostream>
#include <fstream>
#include <string>
#include <algorithm>
#include <sstream>

#include "../system/diagnostics.hpp"
#include "../system/log.hpp"
#include "../system/except.hpp"
#include "../formats/blz.hpp"
#include "../utils/endian.hpp"
#include "../utils/util.hpp"

namespace fs = std::filesystem;

static const char* LoadInf = "Loading ARM| binary...";
static const char* LoadErr7 = "Could not load ARM7.";
static const char* LoadErr9 = "Could not load ARM9.";
static const char* InvResn = "Invalid ARM| file.";

ArmBin::ArmBin() = default;

// Takes the binary's bytes rather than a path: where they came from (a loose
// arm9.bin, a backup copy, or an extent inside a .nds) is the ROM accessor's
// business, and this class has no reason to know which.
void ArmBin::load(std::vector<u8> bytes, u32 entryAddr, u32 ramAddr, u32 autoLoadHookOff, bool isArm9)
{
	m_ramAddr = ramAddr;
	m_entryAddr = entryAddr;
	m_autoLoadHookOff = autoLoadHookOff;
	m_isArm9 = isArm9;

	Log::info(getString(LoadInf));

	ncp::ScopedContext ctx(ncp::Diag::ArmBinLoad, isArm9 ? LoadErr9 : LoadErr7);

	const std::size_t fileSize = bytes.size();
	if (fileSize < 4)
		throw ncp::exception(getString(InvResn));

	m_bytes = std::move(bytes);

	u8* bytesData = m_bytes.data();

	// FIND MODULE PARAMS ================================

	// Every word here is read the way the container code reads a ROM: with
	// shifts, over a bounds-checked span. Pointing a u32* at the module's bytes
	// gets the byte order wrong on a big-endian host, is an aliasing bet the
	// optimizer is free to call, and (since these offsets come out of the
	// file itself) reads off the end of a truncated binary without noticing.
	m_moduleParamsOff = ncp::le::readU32(m_bytes, autoLoadHookOff - m_ramAddr - 4) - m_ramAddr;

	Log::out << OINFO << "Found ModuleParams at: 0x" << std::uppercase << std::hex << m_moduleParamsOff << std::endl;

	// DECOMPRESS ================================

	if (compStaticEnd())
	{
		Log::out << OINFO << "Decompressing..." << std::endl;

		const u32 decompSize = u32(fileSize)
			+ ncp::le::readU32(m_bytes, compStaticEnd() - m_ramAddr - 4);

		m_bytes.resize(decompSize);
		bytesData = m_bytes.data();

		try
		{
			// The image ends at compStaticEnd, which for the ARM9 binary is
			// short of the end of the file: the secure area sits below it and
			// the autoload lists sit above it, and neither is compressed.
			BLZ::uncompressInplace(bytesData, compStaticEnd() - m_ramAddr, m_bytes.size());
		}
		catch (const std::exception& e)
		{
			std::ostringstream oss;
			oss << "Failed to decompress the binary: " << e.what();
			throw ncp::exception(oss.str());
		}

		Log::out << OINFO << "  Old size: 0x" << fileSize << std::endl;
		Log::out << OINFO << "  New size: 0x" << decompSize << std::endl;

		setCompStaticEnd(0);
	}

	// AUTO LOAD ================================

	refreshAutoloadData();
}

void ArmBin::readBytes(u32 address, void* out, u32 size) const
{
	auto failDueToSizeExceed = [&](){
		std::ostringstream oss;
		oss << "Failed to read from arm, reading " << size << " byte(s) from address 0x" <<
			std::uppercase << std::hex << address << std::nouppercase << " exceeds range.";
		throw std::out_of_range(oss.str());
	};

	const u32 autoloadBase = autoloadStart();
	if (address >= m_ramAddr && address < autoloadBase)
	{
		if (address + size > autoloadBase)
			failDueToSizeExceed();
		std::memcpy(out, &m_bytes[address - m_ramAddr], size);
		return;
	}

	for (const AutoLoadEntry& autoload : m_autoloadList)
	{
		u32 autoloadEnd = autoload.address + autoload.size;
		if (address >= autoload.address && address < autoloadEnd)
		{
			if (address + size > autoloadEnd)
				failDueToSizeExceed();
			std::memcpy(out, &m_bytes[autoload.dataOff + (address - autoload.address)], size);
			return;
		}
	}

	std::ostringstream oss;
	oss << "Address 0x" << std::uppercase << std::hex << address << std::nouppercase << " out of range.";
	throw std::out_of_range(oss.str());
}

void ArmBin::writeBytes(u32 address, const void* data, u32 size)
{
	auto failDueToSizeExceed = [&](){
		std::ostringstream oss;
		oss << "Failed to write to arm, writing " << size << " byte(s) to address 0x" <<
			std::uppercase << std::hex << address << std::nouppercase << " exceeds range.";
		throw std::out_of_range(oss.str());
	};

	const u32 autoloadBase = autoloadStart();
	if (address >= m_ramAddr && address < autoloadBase)
	{
		if (address + size > autoloadBase)
			failDueToSizeExceed();
		std::memcpy(&m_bytes[address - m_ramAddr], data, size);
		return;
	}

	for (AutoLoadEntry& autoload : m_autoloadList)
	{
		u32 autoloadEnd = autoload.address + autoload.size;
		if (address >= autoload.address && address < autoloadEnd)
		{
			if (address + size > autoloadEnd)
				failDueToSizeExceed();
			std::memcpy(&m_bytes[autoload.dataOff + (address - autoload.address)], data, size);
			return;
		}
	}

	std::ostringstream oss;
	oss << "Address 0x" << std::uppercase << std::hex << address << std::nouppercase << " out of range.";
	throw std::out_of_range(oss.str());
}

void ArmBin::refreshAutoloadData()
{
	m_autoloadList.clear();

	// The list is three words per entry, read the same way as the words above.
	std::size_t alIter = autoloadListStart() - m_ramAddr;
	const std::size_t alEnd = autoloadListEnd() - m_ramAddr;
	u32 alDataIter = autoloadStart() - m_ramAddr;

	while (alIter < alEnd)
	{
		AutoLoadEntry entry;
		entry.address = ncp::le::readU32(m_bytes, alIter);
		entry.size = ncp::le::readU32(m_bytes, alIter + 4);
		entry.bssSize = ncp::le::readU32(m_bytes, alIter + 8);
		entry.dataOff = alDataIter;

		m_autoloadList.push_back(entry);

		alIter += 12;
		alDataIter += entry.size;
	}
}

// One field of the ModuleParams block. Resolved against m_bytes on every call
// rather than cached, so that resizing the binary (which decompression and
// PatchMaker both do) cannot leave a caller reading freed storage.
u32 ArmBin::moduleParam(u32 fieldOffset) const
{
	return ncp::le::readU32(m_bytes, m_moduleParamsOff + fieldOffset);
}

void ArmBin::setModuleParam(u32 fieldOffset, u32 value)
{
	ncp::le::writeU32(m_bytes, m_moduleParamsOff + fieldOffset, value);
}

std::string ArmBin::getString(const std::string& str) const
{
	return Util::strRepl(str, '|', char('0' + (m_isArm9 ? 9 : 7)));
}
