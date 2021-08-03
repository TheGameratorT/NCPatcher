#pragma once

#include <filesystem>

#include "common.hpp"

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
	u32 compressed : 24; // size of commpressed "ramSize"
	u32 flag : 8;
};

class Overlay
{
public:
	Overlay(const std::filesystem::path& path, u32 ramAddress, bool compressed);
	~Overlay();

	template<typename T>
	T read(u32 address) const {
		return *reinterpret_cast<const T*>(&bytes[address - ramAddress]);
	}

	template<typename T>
	void write(u32 address, T value) {
		*reinterpret_cast<T*>(&bytes[address - ramAddress]) = value;
	}

	constexpr std::vector<u8>& data() { return bytes; };
	constexpr const std::vector<u8>& data() const { return bytes; };

private:
	std::vector<u8> bytes;
	u32 ramAddress;
};
