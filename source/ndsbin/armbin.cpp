#include "armbin.hpp"

#include <iostream>
#include <fstream>
#include <string>
#include <algorithm>

#include "../main.hpp"
#include "../log.hpp"
#include "../except.hpp"
#include "../blz.hpp"
#include "../util.hpp"

namespace fs = std::filesystem;

static const char* LoadInf = "Loading ARM| binary...";
static const char* LoadErr7 = "Could not load ARM7.";
static const char* LoadErr9 = "Could not load ARM9.";
static const char* InvResn = "Invalid ARM| file.";

ArmBin::ArmBin() = default;

void ArmBin::load(const fs::path& path, u32 entryAddr, u32 ramAddr, u32 autoLoadHookOff, bool isArm9)
{
	m_ramAddr = ramAddr;
	m_entryAddr = entryAddr;
	m_autoLoadHookOff = autoLoadHookOff;
	m_isArm9 = isArm9;

	Log::info(getString(LoadInf));

	// READ FILE ================================

	Main::setErrorContext(isArm9 ? LoadErr9 : LoadErr7);

	if (!fs::exists(path))
		throw ncp::file_error(path, ncp::file_error::find);

	uintmax_t fileSize = fs::file_size(path);
	if (fileSize < 4)
		throw ncp::exception(getString(InvResn));

	std::ifstream file(path, std::ios::binary);
	if (!file.is_open())
		throw ncp::file_error(path, ncp::file_error::read);

	m_bytes.resize(fileSize);
	file.read(reinterpret_cast<char*>(m_bytes.data()), std::streamsize(fileSize));
	file.close();

	u8* bytesData = m_bytes.data();

	// FIND MODULE PARAMS ================================

	m_moduleParamsOff = *reinterpret_cast<u32*>(&bytesData[autoLoadHookOff - m_ramAddr - 4]) - m_ramAddr;

	Log::out << OINFO << "Found ModuleParams at: 0x" << std::uppercase << std::hex << m_moduleParamsOff << std::endl;

	ModuleParams* moduleParams = getModuleParams();

	// DECOMPRESS ================================

	if (moduleParams->compStaticEnd)
	{
		Log::out << OINFO << "Decompressing..." << std::endl;

		u32 decompSize = fileSize + *reinterpret_cast<u32*>(&bytesData[moduleParams->compStaticEnd - m_ramAddr - 4]);

		m_bytes.resize(decompSize);
		bytesData = m_bytes.data();
		moduleParams = getModuleParams();

		try
		{
			BLZ::uncompressInplace(&bytesData[moduleParams->compStaticEnd - m_ramAddr]);
		}
		catch (const std::exception& e)
		{
			std::ostringstream oss;
			oss << "Failed to decompress the binary: " << e.what();
			throw ncp::exception(oss.str());
		}

		Log::out << OINFO << "  Old size: 0x" << fileSize << std::endl;
		Log::out << OINFO << "  New size: 0x" << decompSize << std::endl;

		moduleParams->compStaticEnd = 0;
	}

	// AUTO LOAD ================================

	refreshAutoloadData();

	Main::setErrorContext(nullptr);
}

void ArmBin::readBytes(u32 address, void* out, u32 size) const
{
	auto failDueToSizeExceed = [&](){
		std::ostringstream oss;
		oss << "Failed to read from arm, reading " << size << " byte(s) from address 0x" <<
			std::uppercase << std::hex << address << std::nouppercase << " exceeds range.";
		throw std::out_of_range(oss.str());
	};

	u32 autoloadStart = getModuleParams()->autoloadStart;
	if (address >= m_ramAddr && address < autoloadStart)
	{
		if (address + size > autoloadStart)
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

	u32 autoloadStart = getModuleParams()->autoloadStart;
	if (address >= m_ramAddr && address < autoloadStart)
	{
		if (address + size > autoloadStart)
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
	u8* bytesData = m_bytes.data();
	ModuleParams* moduleParams = getModuleParams();

	m_autoloadList.clear();

	u32* alIter = reinterpret_cast<u32*>(&bytesData[moduleParams->autoloadListStart - m_ramAddr]);
	u32* alEnd = reinterpret_cast<u32*>(&bytesData[moduleParams->autoloadListEnd - m_ramAddr]);
	u32 alDataIter = moduleParams->autoloadStart - m_ramAddr;

	while (alIter < alEnd)
	{
		u32 entryInfo[3];
		std::memcpy(&entryInfo, alIter, 12);

		AutoLoadEntry entry;
		entry.address = entryInfo[0];
		entry.size = entryInfo[1];
		entry.bssSize = entryInfo[2];
		entry.dataOff = alDataIter;

		m_autoloadList.push_back(entry);

		alIter += 3;
		alDataIter += entry.size;
	}
}

std::string ArmBin::getString(const std::string& str) const
{
	return Util::strRepl(str, '|', char('0' + (m_isArm9 ? 9 : 7)));
}
