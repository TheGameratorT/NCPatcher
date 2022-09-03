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
	this->ramAddr = ramAddr;
	this->entryAddr = entryAddr;
	this->autoLoadHookOff = autoLoadHookOff;
	this->isArm9 = isArm9;

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

	bytes.resize(fileSize);
	file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(fileSize));
	file.close();

	u8* bytesData = bytes.data();

	// FIND MODULE PARAMS ================================

	moduleParamsOff = *reinterpret_cast<u32*>(&bytesData[autoLoadHookOff - ramAddr - 4]) - ramAddr;

	Log::out << OINFO << "Found ModuleParams at: 0x" << std::uppercase << std::hex << moduleParamsOff << std::endl;
	memcpy(&moduleParams, &bytesData[moduleParamsOff], sizeof(ModuleParams));

	// DECOMPRESS ================================

	if (moduleParams.compStaticEnd)
	{
		Log::out << OINFO << "Decompressing..." << std::endl;

		u32 decompSize = fileSize + *reinterpret_cast<u32*>(&bytesData[moduleParams.compStaticEnd - ramAddr - 4]);

		bytes.resize(decompSize);
		bytesData = bytes.data();

		try
		{
			BLZ::uncompress(&bytesData[moduleParams.compStaticEnd - ramAddr]);
		}
		catch (const std::exception& e)
		{
			std::ostringstream oss;
			oss << "Failed to decompress the binary: " << e.what();
			throw ncp::exception(oss.str());
		}

		Log::out << OINFO << "  Old size: 0x" << fileSize << std::endl;
		Log::out << OINFO << "  New size: 0x" << decompSize << std::endl;

		moduleParams.compStaticEnd = 0;
		*reinterpret_cast<u32*>(&bytesData[moduleParamsOff + 20]) = 0;
	}

	// AUTO LOAD ================================

	u32* alIter = reinterpret_cast<u32*>(&bytesData[moduleParams.autoloadListStart - ramAddr]);
	u32* alEnd = reinterpret_cast<u32*>(&bytesData[moduleParams.autoloadListEnd - ramAddr]);
	u8* alDataIter = &bytesData[moduleParams.autoloadStart - ramAddr];

	while (alIter < alEnd)
	{
		// TODO: More safety on AutoLoadEntry loading
		AutoLoadEntry entry;
		memcpy(&entry, alIter, 12);

		entry.data.resize(entry.size);
		memcpy(entry.data.data(), alDataIter, entry.size);

		autoloadList.push_back(entry);

		alIter += 3;
		alDataIter += entry.size;
	}

	Main::setErrorContext(nullptr);
}

std::vector<u8>& ArmBin::data()
{
	return bytes;
}

std::string ArmBin::getString(const std::string& str) const
{
	return Util::strRepl(str, '|', char('0' + (isArm9 ? 9 : 7)));
}
