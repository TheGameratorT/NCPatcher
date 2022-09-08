#pragma once

#include <vector>
#include <filesystem>

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

class OverlayBin
{
public:
	OverlayBin();

	void load(const std::filesystem::path& path, u32 ramAddress, bool compressed);

	template<typename T>
	T read(u32 address) const {
		return *reinterpret_cast<const T*>(&m_bytes[address - m_ramAddress]);
	}

	template<typename T>
	void write(u32 address, T value) {
		*reinterpret_cast<T*>(&m_bytes[address - m_ramAddress]) = value;
	}

	constexpr std::vector<u8>& data() { return m_bytes; };
	[[nodiscard]] constexpr const std::vector<u8>& data() const { return m_bytes; };

private:
	std::vector<u8> m_bytes;
	u32 m_ramAddress;
};
