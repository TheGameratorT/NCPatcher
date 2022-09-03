#pragma once

#include <sstream>
#include <filesystem>
#include <vector>
#include <exception>

#include "../types.hpp"

class ArmBin
{
public:
	ArmBin();
	void load(const std::filesystem::path& path, u32 entryAddr, u32 ramAddr, u32 autoLoadHookOff, bool isArm9);

	template<typename T>
	T read(u32 address) const
	{
		if (address >= ramAddr && address < moduleParams.autoloadStart)
			return *reinterpret_cast<const T*>(&bytes[address - ramAddr]);

		for (const AutoLoadEntry& autoload : autoloadList)
		{
			if (address >= autoload.address && address < autoload.address + autoload.size)
				return *reinterpret_cast<const T*>(&autoload.data[address - autoload.address]);
		}

		std::ostringstream oss;
		oss << "Address 0x" << std::uppercase << std::hex << address << std::nouppercase << " out of range.";
		throw std::out_of_range(oss.str());
	}

	template<typename T>
	void write(u32 address, T value)
	{
		if (address >= ramAddr && address < moduleParams.autoloadStart)
		{
			*reinterpret_cast<const T*>(&bytes[address - ramAddr]) = value;
			return;
		}

		for (const AutoLoadEntry& autoload : autoloadList)
		{
			if (address >= autoload.address && address < autoload.address + autoload.size)
			{
				*reinterpret_cast<const T*>(&autoload.data[address - autoload.address]) = value;
				return;
			}
		}

		std::ostringstream oss;
		oss << "Address 0x" << std::uppercase << std::hex << address << std::nouppercase << " out of range.";
		throw std::out_of_range(oss.str());
	}

	std::vector<u8>& data();

private:
	struct ModuleParams
	{
		u32 autoloadListStart;
		u32 autoloadListEnd;
		u32 autoloadStart;
		u32 staticBssStart;
		u32 staticBssEnd;
		u32 compStaticEnd; //compressedStaticEnd
		u32 sdkVersionID;
		u32 nitroCodeBE;
		u32 nitroCodeLE;
	};

	struct AutoLoadEntry
	{
		u32 address;
		u32 size;
		u32 bssSize;
		std::vector<u8> data;
	};

	u32 ramAddr; //The offset of this binary in memory
	u32 entryAddr; //The address of the entry point
	u32 autoLoadHookOff;
	u32 moduleParamsOff;
	u32 isArm9;

	ModuleParams moduleParams;
	std::vector<AutoLoadEntry> autoloadList;

	std::vector<u8> bytes;

	std::string getString(const std::string& str) const;
};
