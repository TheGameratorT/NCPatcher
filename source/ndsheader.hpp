#pragma once

#include <filesystem>

#include "common.hpp"

class NDSHeader
{
public:
	void load(const std::filesystem::path& path);

	struct ARMBinaryInfo
	{
		u32 romOffset;
		u32 entryAddress;
		u32 ramAddress;
		u32 size;
	};

	struct DebugBinaryInfo
	{
		u32 romOffset;
		u32 size;
		u32 ramAddress;
	};

	struct BinaryInfo
	{
		u32 romOffset;
		u32 size;
	};

	char gameTitle[12];
	char gameCode[4];
	char makerCode[2];
	u8 unitCode;
	u8 encSeedSel;
	u8 deviceCapacity;
	u8 reserved1[8];
	u8 ndsRegion;
	u8 romVersion;
	u8 autoStart;
	ARMBinaryInfo arm9;
	ARMBinaryInfo arm7;
	BinaryInfo fnt;
	BinaryInfo fat;
	BinaryInfo arm9OvT;
	BinaryInfo arm7OvT;
	u32 normalPortCmdSet;
	u32 key1PortCmdSet;
	u32 bannerOffset;
	u16 secureAreaChecksum;
	u16 secureAreaDelay;
	u32 arm9AutoLoadListHookOffset;
	u32 arm7AutoLoadListHookOffset;
	u64 secureAreaDisable;
	u32 totalUsedRomSize;
	u32 romHeaderSize;
	u8 reserved2[56];
	u8 nintendoLogo[156];
	u16 nintendoLogoCheckSum;
	u16 headerChecksum;
	DebugBinaryInfo debug;
	u8 reserved3[148];
};
