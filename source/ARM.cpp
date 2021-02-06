#include "ARM.hpp"

#include <iostream>
#include <fstream>

#include "NCException.hpp"
#include "BLZ.hpp"

namespace fs = std::filesystem;

constexpr int NitroCodeLE = 0x2106C0DE;
constexpr int NitroCodeBE = 0xDEC00621;

static const char* LoadInf = "Loading ARM|...";
static const char* LoadErr = "Could not load ARM|.";
static const char* InvResn = "Invalid ARM| file.";

ARM::ARM(const std::filesystem::path& path, u32 entryAddr, u32 ramAddr, u32 target)
{
	this->ramAddr = ramAddr;
	this->entryAddr = entryAddr;
	this->target = target;

	load(path);
}

void ARM::load(const fs::path& path)
{
	std::cout << OINFO << getString(LoadInf) << std::endl;

	// READ FILE ================================

	std::string pathQ = path.string();

	if (!fs::exists(path))
	{
		throw NC::file_error(getString(LoadErr), path, NC::file_error::find);
	}

	uintmax_t fileSize = fs::file_size(path);
	if (fileSize < 4)
	{
		throw NC::exception(getString(LoadErr), getString(InvResn));
	}

	std::ifstream file(path, std::ios::binary);
	if (!file.is_open())
	{
		throw NC::file_error(getString(LoadErr), path, NC::file_error::read);
	}

	bytes.resize(fileSize);
	file.read(reinterpret_cast<char*>(bytes.data()), fileSize);
	file.close();

	// FIND MODULE PARAMS ================================

	u32 entryOff = entryAddr - ramAddr;

	moduleParamsOff = 0;
	for (int i = entryOff; (i < fileSize - 4) && (i < (entryOff + 0x400)); i += 4)
	{
		u32 be = *reinterpret_cast<u32*>(&bytes[i]);
		u32 le = *reinterpret_cast<u32*>(&bytes[i + 4]);

		if (be == NitroCodeBE && le == NitroCodeLE)
		{
			moduleParamsOff = i - 0x1C;
			break;
		}
	}

	if (!moduleParamsOff) // ModuleParams will never be 0 unless some troll moves it there.
	{
		throw NC::exception(getString(LoadErr), "Unable to find ModuleParams.");
	}

	std::cout << OINFO << "Found ModuleParams at: 0x" << std::uppercase << std::hex << moduleParamsOff << std::endl;
	memcpy(&moduleParams, &bytes[moduleParamsOff], sizeof(ModuleParams));

	// DECOMPRESS ================================

	if (moduleParams.compStaticEnd)
	{
		std::cout << OINFO << "Decompressing..." << std::endl;

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
			throw NC::exception(getString(LoadErr), oss.str());
		}

		std::cout << OINFO << "  Old size: 0x" << fileSize << std::endl;
		std::cout << OINFO << "  New size: 0x" << decompSize << std::endl;

		moduleParams.compStaticEnd = 0;
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

	// SAVE DEBUG BACKUP ================================

	fs::path patho = path.parent_path() / "backup" / path.filename();
	std::ofstream fileo(patho, std::ios::binary);
	fileo.write(reinterpret_cast<char*>(bytes.data()), bytes.size());
	fileo.close();
}

std::string ARM::getString(const std::string& str)
{
	return Util::str_repl(str, '|', '0' + target);
}
