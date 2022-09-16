#pragma once

#include "../types.hpp"

class ICodeBin
{
public:
	virtual void readBytes(u32 address, void* out, u32 size) const = 0;
	virtual void writeBytes(u32 address, const void* data, u32 size) = 0;

	template<typename T>
	T read(u32 address) const {
		T value;
		readBytes(address, &value, sizeof(T));
		return value;
	}

	template<typename T>
	void write(u32 address, T value) {
		writeBytes(address, &value, sizeof(T));
	}
};
