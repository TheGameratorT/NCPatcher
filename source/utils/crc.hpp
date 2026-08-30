#pragma once

// CRC-16/MODBUS, the checksum the DS cartridge header uses.
//
// Reflected polynomial 0xA001 (0x8005 read the other way round), initial value
// 0xFFFF, no final inversion. The header stores three of these: over the
// Nintendo logo, over the secure area, and over the header's own first 0x15E
// bytes. Only the last is ours to recompute; see rom/header.cpp.

#include <cstddef>
#include <span>

#include "types.hpp"

namespace Crc {

[[nodiscard]] inline u16 modbus16(std::span<const u8> data, u16 initial = 0xFFFF)
{
	u16 crc = initial;
	for (u8 byte : data)
	{
		crc ^= byte;
		for (int bit = 0; bit < 8; bit++)
			crc = (crc & 1) ? u16((crc >> 1) ^ 0xA001) : u16(crc >> 1);
	}
	return crc;
}

} // namespace Crc
