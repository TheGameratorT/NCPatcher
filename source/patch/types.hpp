#pragma once

#include <vector>
#include <memory>
#include <string>

#include "../utils/types.hpp"
#include "../core/compilation_unit.hpp"

namespace ncp::patch {

constexpr size_t PatchTypeCount = 14;

// Adjust patchTypeNames and PatchTypeCount if changed
enum class PatchType : u32 {
	Jump, Call, Hook,
	SetJump, SetCall, SetHook,
	TJump, TCall, THook,
	SetTJump, SetTCall, SetTHook,
	Over,
	RtRepl,

	Invalid = 0xFFFFFFFF
};

enum class PatchOrigin {
	Section,
	Symbol,
	Symver
};

struct PatchInfo {
	core::CompilationUnit* unit;
	PatchType type;
	PatchOrigin origin;
	std::string symbol;
	u32 srcAddress;
	int srcAddressOv;
	u32 destAddress;
	int destAddressOv;
	bool srcThumb;
	bool destThumb;
	u16 sectionIdx;
	u32 sectionSize;
	bool isNcpSet;

	std::string getPrettyName() const;

	// Returns how many bytes this patch will write to the destination
	u32 getOverwriteAmount() const;
};

struct SectionInfo {
    core::CompilationUnit* unit;
    std::string name;
    std::size_t size;
    u32 alignment;
    u32 address;
    const u8* data;
};

struct NewcodeInfo {
    const u8* binData;
    const u8* bssData;
    std::size_t binSize;
    std::size_t binAlign;
    std::size_t bssSize;
    std::size_t bssAlign;
};

struct AutogenDataInfo {
    u32 address;
    u32 curAddress;
    std::vector<u8> data;
};

struct OverwriteRegionInfo
{
    std::string name;
    u32 startAddress;
    u32 endAddress;
    int destination;
    std::vector<SectionInfo*> assignedSections;
    std::vector<PatchInfo*> sectionPatches;
    u32 usedSize;
    int sectionIdx;
    int sectionSize;
};

const char* toString(PatchType type);
const char* toString(PatchOrigin origin);

PatchType patchTypeFromString(std::string_view patchTypeName);

} // namespace ncp::patch
