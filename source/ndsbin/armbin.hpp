#pragma once

#include <sstream>
#include <filesystem>
#include <vector>
#include <exception>

#include "icodebin.hpp"

#include "../utils/types.hpp"

class ArmBin : public ICodeBin
{
public:
	// Offsets of the fields within the ModuleParams block, which is nine
	// little-endian words sitting inside the binary's own bytes. The block is
	// addressed by offset rather than overlaid with a struct: a struct would
	// read the words in host order, would let the compiler decide the layout,
	// and would hand out a pointer into a vector that this class resizes.
	struct ModuleParams
	{
		static constexpr u32 AutoloadListStart = 0x00;
		static constexpr u32 AutoloadListEnd   = 0x04;
		static constexpr u32 AutoloadStart     = 0x08;
		static constexpr u32 StaticBssStart    = 0x0C;
		static constexpr u32 StaticBssEnd      = 0x10;
		static constexpr u32 CompStaticEnd     = 0x14; //compressedStaticEnd
		static constexpr u32 SdkVersionId      = 0x18;
		static constexpr u32 NitroCodeBE       = 0x1C;
		static constexpr u32 NitroCodeLE       = 0x20;
		static constexpr u32 Size              = 0x24;
	};

	struct AutoLoadEntry
	{
		u32 address;
		u32 size;
		u32 bssSize;
		u32 dataOff;
	};

	ArmBin();
	void load(std::vector<u8> bytes, u32 entryAddr, u32 ramAddr, u32 autoLoadHookOff, bool isArm9);

	void readBytes(u32 address, void* out, u32 size) const override;
	void writeBytes(u32 address, const void* data, u32 size) override;

	void refreshAutoloadData();

	[[nodiscard]] constexpr u32 getRamAddress() const { return m_ramAddr; }
	[[nodiscard]] constexpr bool isArm9() const { return m_isArm9; }
	[[nodiscard]] u32 autoloadListStart() const { return moduleParam(ModuleParams::AutoloadListStart); }
	[[nodiscard]] u32 autoloadListEnd() const   { return moduleParam(ModuleParams::AutoloadListEnd); }
	[[nodiscard]] u32 autoloadStart() const     { return moduleParam(ModuleParams::AutoloadStart); }
	[[nodiscard]] u32 staticBssStart() const    { return moduleParam(ModuleParams::StaticBssStart); }
	[[nodiscard]] u32 staticBssEnd() const      { return moduleParam(ModuleParams::StaticBssEnd); }
	[[nodiscard]] u32 compStaticEnd() const     { return moduleParam(ModuleParams::CompStaticEnd); }

	void setAutoloadListStart(u32 value) { setModuleParam(ModuleParams::AutoloadListStart, value); }
	void setAutoloadListEnd(u32 value)   { setModuleParam(ModuleParams::AutoloadListEnd, value); }
	void setCompStaticEnd(u32 value)     { setModuleParam(ModuleParams::CompStaticEnd, value); }
	[[nodiscard]] constexpr std::vector<AutoLoadEntry>& getAutoloadList() { return m_autoloadList; }
	[[nodiscard]] constexpr const std::vector<AutoLoadEntry>& getAutoloadList() const { return m_autoloadList; }
	[[nodiscard]] constexpr std::vector<u8>& data() { return m_bytes; }
	[[nodiscard]] constexpr const std::vector<u8>& data() const { return m_bytes; }
	[[nodiscard]] constexpr bool sanityCheckAddress(u32 addr) const { return addr >= m_ramAddr && addr < (m_ramAddr + 0x00400000); }

private:
	u32 m_ramAddr; //The offset of this binary in memory
	u32 m_entryAddr; //The address of the entry point
	u32 m_autoLoadHookOff;
	u32 m_moduleParamsOff;
	u32 m_isArm9;

	std::vector<u8> m_bytes;
	std::vector<AutoLoadEntry> m_autoloadList;

	[[nodiscard]] u32 moduleParam(u32 fieldOffset) const;
	void setModuleParam(u32 fieldOffset, u32 value);

	std::string getString(const std::string& str) const;
};
