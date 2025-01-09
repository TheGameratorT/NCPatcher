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
		u32 dataOff;
	};

	ArmBin();
	void load(const std::filesystem::path& path, u32 entryAddr, u32 ramAddr, u32 autoLoadHookOff, bool isArm9);

	void readBytes(u32 address, void* out, u32 size) const override;
	void writeBytes(u32 address, const void* data, u32 size) override;

	void refreshAutoloadData();

	[[nodiscard]] constexpr u32 getRamAddress() const { return m_ramAddr; }
	[[nodiscard]] inline ModuleParams* getModuleParams() { return reinterpret_cast<ModuleParams*>(&((m_bytes.data())[m_moduleParamsOff])); }
	[[nodiscard]] inline const ModuleParams* getModuleParams() const { return reinterpret_cast<const ModuleParams*>(&((m_bytes.data())[m_moduleParamsOff])); }
	[[nodiscard]] constexpr std::vector<AutoLoadEntry>& getAutoloadList() { return m_autoloadList; }
	[[nodiscard]] constexpr const std::vector<AutoLoadEntry>& getAutoloadList() const { return m_autoloadList; }
	[[nodiscard]] constexpr std::vector<u8>& data() { return m_bytes; }
	[[nodiscard]] constexpr const std::vector<u8>& data() const { return m_bytes; }
	[[nodiscard]] constexpr const bool sanityCheckAddress(u32 addr) { return addr >= m_ramAddr && addr < (m_ramAddr + 0x00400000); }

private:
	u32 m_ramAddr; //The offset of this binary in memory
	u32 m_entryAddr; //The address of the entry point
	u32 m_autoLoadHookOff;
	u32 m_moduleParamsOff;
	u32 m_isArm9;

	std::vector<u8> m_bytes;
	std::vector<AutoLoadEntry> m_autoloadList;

	std::string getString(const std::string& str) const;
};
