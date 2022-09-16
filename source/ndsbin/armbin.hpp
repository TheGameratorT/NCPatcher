#pragma once

#include <sstream>
#include <filesystem>
#include <vector>
#include <exception>

#include "icodebin.hpp"

#include "../types.hpp"

class ArmBin : public ICodeBin
{
public:
	ArmBin();
	void load(const std::filesystem::path& path, u32 entryAddr, u32 ramAddr, u32 autoLoadHookOff, bool isArm9);

	void readBytes(u32 address, void* out, u32 size) const override;
	void writeBytes(u32 address, const void* data, u32 size) override;

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

	u32 m_ramAddr; //The offset of this binary in memory
	u32 m_entryAddr; //The address of the entry point
	u32 m_autoLoadHookOff;
	u32 m_moduleParamsOff;
	u32 m_isArm9;

	ModuleParams m_moduleParams;
	std::vector<AutoLoadEntry> m_autoloadList;

	std::vector<u8> m_bytes;

	std::string getString(const std::string& str) const;
};
