#pragma once

#include <array>
#include <span>

#include "../utils/endian.hpp"
#include "../utils/types.hpp"

class ICodeBin
{
public:
	virtual void readBytes(u32 address, void* out, u32 size) const = 0;
	virtual void writeBytes(u32 address, const void* data, u32 size) = 0;

	// The word at `address` is a word of an ARM binary, so it is little-endian
	// whatever the host is. Going through the codec rather than memcpying a T
	// keeps that true, and keeps these in step with every other reader of the
	// same bytes.
	template<typename T>
	T read(u32 address) const {
		std::array<u8, sizeof(T)> bytes;
		readBytes(address, bytes.data(), sizeof(T));
		return T(ncp::le::readUInt<sizeof(T)>(bytes));
	}

	template<typename T>
	void write(u32 address, T value) {
		std::array<u8, sizeof(T)> bytes;
		ncp::le::writeUInt<sizeof(T)>(bytes, value);
		writeBytes(address, bytes.data(), sizeof(T));
	}
};
