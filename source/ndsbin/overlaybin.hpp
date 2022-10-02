#pragma once

#include <vector>
#include <filesystem>

#include "icodebin.hpp"

#include "../types.hpp"

#define OVERLAY_FLAG_COMP 1
#define OVERLAY_FLAG_AUTH 2

struct OvtEntry
{
	u32 overlayID;
	u32 ramAddress;
	u32 ramSize;
	u32 bssSize;
	u32 sinitStart;
	u32 sinitEnd;
	u32 fileID;
	u32 compressed : 24; // size of compressed "ramSize"
	u32 flag : 8;
};

class OverlayBin : public ICodeBin
{
public:
	OverlayBin();

	void load(const std::filesystem::path& path, u32 ramAddress, bool compressed, int id);

	void readBytes(u32 address, void* out, u32 size) const override;
	void writeBytes(u32 address, const void* data, u32 size) override;

	[[nodiscard]] constexpr std::vector<u8>& data() { return m_bytes; };
	[[nodiscard]] constexpr const std::vector<u8>& data() const { return m_bytes; };

	[[nodiscard]] constexpr bool getDirty() const { return m_isDirty; }
	constexpr void setDirty(bool isDirty) { m_isDirty = isDirty; }

private:
	std::vector<u8> m_bytes;
	u32 m_ramAddress;
	int m_id;
	bool m_isDirty;
};
