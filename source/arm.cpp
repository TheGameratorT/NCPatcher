#include "arm.hpp"

#include <iostream>
#include <fstream>
#include <string>
#include <algorithm>

#include "global.hpp"
#include "oansistream.hpp"
#include "except.hpp"
#include "blz.hpp"

namespace fs = std::filesystem;

static const char* LoadInf = "Loading ARM|...";
static const char* LoadErr = "Could not load ARM|.";
static const char* InvResn = "Invalid ARM| file.";

ARM::ARM(const fs::path& path, u32 entryAddr, u32 ramAddr, u32 autoLoadHookOff, u32 target)
{
	this->ramAddr = ramAddr;
	this->entryAddr = entryAddr;
	this->autoLoadHookOff = autoLoadHookOff;
	this->target = target;

	load(path);
}

void ARM::load(const fs::path& path)
{
	ansi::cout << OINFO << getString(LoadInf) << std::endl;

	// READ FILE ================================

	ncp::setErrorMsg(getString(LoadErr));

	if (!fs::exists(path))
		throw ncp::file_error(path, ncp::file_error::find);

	uintmax_t fileSize = fs::file_size(path);
	if (fileSize < 4)
		throw ncp::exception(getString(InvResn));

	std::ifstream file(path, std::ios::binary);
	if (!file.is_open())
		throw ncp::file_error(path, ncp::file_error::read);

	bytes.resize(fileSize);
	file.read(reinterpret_cast<char*>(bytes.data()), fileSize);
	file.close();

	// FIND MODULE PARAMS ================================

	moduleParamsOff = *reinterpret_cast<u32*>(&bytes[autoLoadHookOff - ramAddr - 4]) - ramAddr;

	ansi::cout << OINFO << "Found ModuleParams at: 0x" << std::uppercase << std::hex << moduleParamsOff << std::endl;
	memcpy(&moduleParams, &bytes[moduleParamsOff], sizeof(ModuleParams));

	// DECOMPRESS ================================

	if (moduleParams.compStaticEnd)
	{
		ansi::cout << OINFO << "Decompressing..." << std::endl;

		u32 decompSize = fileSize + *reinterpret_cast<u32*>(&bytes[moduleParams.compStaticEnd - ramAddr - 4]);

		bytes.resize(decompSize);

		try
		{
			BLZ::uncompress(&bytes[moduleParams.compStaticEnd - ramAddr]);
		}
		catch (const std::exception& e)
		{
			std::ostringstream oss;
			oss << "Failed to decompress the binary: " << e.what();
			throw ncp::exception(oss.str());
		}

		ansi::cout << OINFO << "  Old size: 0x" << fileSize << std::endl;
		ansi::cout << OINFO << "  New size: 0x" << decompSize << std::endl;

		moduleParams.compStaticEnd = 0;
		*reinterpret_cast<u32*>(&bytes[moduleParamsOff + 20]) = 0;
	}

	// AUTO LOAD ================================

	u32* alIter = reinterpret_cast<u32*>(&bytes[moduleParams.autoloadListStart - ramAddr]);
	u32* alEnd = reinterpret_cast<u32*>(&bytes[moduleParams.autoloadListEnd - ramAddr]);
	u8* alDataIter = &bytes[moduleParams.autoloadStart - ramAddr];

	while (alIter < alEnd)
	{
		AutoLoadEntry entry;
		memcpy(&entry, alIter, 12);

		entry.data.resize(entry.size);
		memcpy(entry.data.data(), alDataIter, entry.size);

		autoloadList.push_back(entry);

		alIter += 3;
		alDataIter += entry.size;
	}
}

std::vector<u8>& ARM::data()
{
	return bytes;
}

std::string ARM::getString(const std::string& str)
{
	return util::str_repl(str, '|', '0' + target);
}
