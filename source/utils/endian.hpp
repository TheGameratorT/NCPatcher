#pragma once

// Little-endian field access over a byte range.
//
// Everything a DS cartridge holds is little-endian and unaligned-tolerant: the
// ROM containers, the ARM binaries inside them, the BLZ footer, and the opcodes
// this tool assembles into them. None of it may be read by pointing a struct or
// a u32* at the bytes: that gets the byte order wrong on a big-endian host,
// gets the layout wrong wherever the compiler inserts padding, is an aliasing
// bet the optimiser is free to call, and -- for the overlay table's 24-bit size
// field -- leaves the bit order entirely up to the implementation. Shifts have
// none of those degrees of freedom, so every one of those readers uses them.
//
// Reads and writes are bounds-checked. A truncated file is something someone
// hands the tool, not a programming error, so it has to produce a message
// rather than a segfault.

#include <cstddef>
#include <span>
#include <stdexcept>
#include <string>

#include "types.hpp"

namespace ncp::le {

[[noreturn]] void throwOutOfRange(std::size_t offset, std::size_t size, std::size_t available);

inline void requireRange(std::span<const u8> data, std::size_t offset, std::size_t size)
{
	// Written as a subtraction so that an offset near SIZE_MAX cannot wrap the
	// sum back into the valid range.
	if (offset > data.size() || size > data.size() - offset)
		throwOutOfRange(offset, size, data.size());
}

[[nodiscard]] inline u8 readU8(std::span<const u8> data, std::size_t offset)
{
	requireRange(data, offset, 1);
	return data[offset];
}

[[nodiscard]] inline u16 readU16(std::span<const u8> data, std::size_t offset)
{
	requireRange(data, offset, 2);
	return u16(u32(data[offset]) | (u32(data[offset + 1]) << 8));
}

[[nodiscard]] inline u32 readU24(std::span<const u8> data, std::size_t offset)
{
	requireRange(data, offset, 3);
	return u32(data[offset]) | (u32(data[offset + 1]) << 8) | (u32(data[offset + 2]) << 16);
}

[[nodiscard]] inline u32 readU32(std::span<const u8> data, std::size_t offset)
{
	requireRange(data, offset, 4);
	return u32(data[offset]) | (u32(data[offset + 1]) << 8) |
	       (u32(data[offset + 2]) << 16) | (u32(data[offset + 3]) << 24);
}

[[nodiscard]] inline u64 readU64(std::span<const u8> data, std::size_t offset)
{
	requireRange(data, offset, 8);
	return u64(readU32(data, offset)) | (u64(readU32(data, offset + 4)) << 32);
}

inline void writeU8(std::span<u8> data, std::size_t offset, u8 value)
{
	requireRange(data, offset, 1);
	data[offset] = value;
}

inline void writeU16(std::span<u8> data, std::size_t offset, u16 value)
{
	requireRange(data, offset, 2);
	data[offset]     = u8(value & 0xFF);
	data[offset + 1] = u8((value >> 8) & 0xFF);
}

inline void writeU24(std::span<u8> data, std::size_t offset, u32 value)
{
	requireRange(data, offset, 3);
	data[offset]     = u8(value & 0xFF);
	data[offset + 1] = u8((value >> 8) & 0xFF);
	data[offset + 2] = u8((value >> 16) & 0xFF);
}

inline void writeU32(std::span<u8> data, std::size_t offset, u32 value)
{
	requireRange(data, offset, 4);
	data[offset]     = u8(value & 0xFF);
	data[offset + 1] = u8((value >> 8) & 0xFF);
	data[offset + 2] = u8((value >> 16) & 0xFF);
	data[offset + 3] = u8((value >> 24) & 0xFF);
}

inline void writeU64(std::span<u8> data, std::size_t offset, u64 value)
{
	requireRange(data, offset, 8);
	writeU32(data, offset, u32(value & 0xFFFFFFFF));
	writeU32(data, offset + 4, u32((value >> 32) & 0xFFFFFFFF));
}

// Width-generic forms, for code that is templated on the field type rather
// than naming it. They are dispatches onto the functions above so that the
// byte order still has exactly one definition.
template<std::size_t N>
[[nodiscard]] inline auto readUInt(std::span<const u8> data, std::size_t offset = 0)
{
	static_assert(N == 1 || N == 2 || N == 4 || N == 8, "no little-endian codec for this width");
	if constexpr (N == 1) return readU8(data, offset);
	else if constexpr (N == 2) return readU16(data, offset);
	else if constexpr (N == 4) return readU32(data, offset);
	else return readU64(data, offset);
}

template<std::size_t N>
inline void writeUInt(std::span<u8> data, u64 value, std::size_t offset = 0)
{
	static_assert(N == 1 || N == 2 || N == 4 || N == 8, "no little-endian codec for this width");
	if constexpr (N == 1) writeU8(data, offset, u8(value));
	else if constexpr (N == 2) writeU16(data, offset, u16(value));
	else if constexpr (N == 4) writeU32(data, offset, u32(value));
	else writeU64(data, offset, value);
}

} // namespace ncp::le
