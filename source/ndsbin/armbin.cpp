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

	m_moduleParamsOff = *reinterpret_cast<u32*>(&bytesData[autoLoadHookOff - ramAddr - 4]) - ramAddr;

	Log::out << OINFO << "Found ModuleParams at: 0x" << std::uppercase << std::hex << m_moduleParamsOff << std::endl;
	memcpy(&m_moduleParams, &bytesData[m_moduleParamsOff], sizeof(ModuleParams));

	// DECOMPRESS ================================

	if (m_moduleParams.compStaticEnd)
	{
		Log::out << OINFO << "Decompressing..." << std::endl;

		u32 decompSize = fileSize + *reinterpret_cast<u32*>(&bytesData[m_moduleParams.compStaticEnd - ramAddr - 4]);

		m_bytes.resize(decompSize);
		bytesData = m_bytes.data();

		try
		{
			BLZ::uncompressInplace(&bytesData[m_moduleParams.compStaticEnd - ramAddr]);
		}
		catch (const std::exception& e)
		{
			std::ostringstream oss;
			oss << "Failed to decompress the binary: " << e.what();
			throw ncp::exception(oss.str());
		}

		Log::out << OINFO << "  Old size: 0x" << fileSize << std::endl;
		Log::out << OINFO << "  New size: 0x" << decompSize << std::endl;

		m_moduleParams.compStaticEnd = 0;
		*reinterpret_cast<u32*>(&bytesData[m_moduleParamsOff + 20]) = 0;
	}

	// AUTO LOAD ================================

	u32* alIter = reinterpret_cast<u32*>(&bytesData[m_moduleParams.autoloadListStart - ramAddr]);
	u32* alEnd = reinterpret_cast<u32*>(&bytesData[m_moduleParams.autoloadListEnd - ramAddr]);
	u8* alDataIter = &bytesData[m_moduleParams.autoloadStart - ramAddr];

	while (alIter < alEnd)
	{
		// TODO: More safety on AutoLoadEntry loading
		AutoLoadEntry entry;
		memcpy(&entry, alIter, 12);

		entry.data.resize(entry.size);
		memcpy(entry.data.data(), alDataIter, entry.size);

		m_autoloadList.push_back(entry);

		alIter += 3;
		alDataIter += entry.size;
	}

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

	if (address >= m_ramAddr && address < m_moduleParams.autoloadStart)
	{
		u32 binAddress = address - m_ramAddr;
		if (binAddress + size > m_moduleParams.autoloadStart)
			failDueToSizeExceed();
		std::memcpy(out, &m_bytes[binAddress], size);
		return;
	}

	for (const AutoLoadEntry& autoload : m_autoloadList)
	{
		u32 autoloadEnd = autoload.address + autoload.size;
		if (address >= autoload.address && address < autoloadEnd)
		{
			u32 binAddress = address - autoload.address;
			if (binAddress + size > autoloadEnd)
				failDueToSizeExceed();
			std::memcpy(out, &autoload.data[binAddress], size);
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

	if (address >= m_ramAddr && address < m_moduleParams.autoloadStart)
	{
		u32 binAddress = address - m_ramAddr;
		if (binAddress + size > m_moduleParams.autoloadStart)
			failDueToSizeExceed();
		std::memcpy(&m_bytes[binAddress], data, size);
		return;
	}

	for (AutoLoadEntry& autoload : m_autoloadList)
	{
		u32 autoloadEnd = autoload.address + autoload.size;
		if (address >= autoload.address && address < autoloadEnd)
		{
			u32 binAddress = address - autoload.address;
			if (binAddress + size > autoloadEnd)
				failDueToSizeExceed();
			std::memcpy(&autoload.data[binAddress], data, size);
			return;
		}
	}

	std::ostringstream oss;
	oss << "Address 0x" << std::uppercase << std::hex << address << std::nouppercase << " out of range.";
	throw std::out_of_range(oss.str());
}

std::vector<u8>& ArmBin::data()
{
	return m_bytes;
}

std::string ArmBin::getString(const std::string& str) const
{
	return Util::strRepl(str, '|', char('0' + (m_isArm9 ? 9 : 7)));
}
