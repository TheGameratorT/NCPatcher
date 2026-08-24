#pragma once

// Little-endian field access over a byte range.
//
// Every structure in a DS ROM is little-endian and unaligned-tolerant, and none
// of them may be read by pointing a struct at the file's bytes: that gets the
// byte order wrong on a big-endian host, gets the layout wrong wherever the
// compiler inserts padding, and -- for the overlay table's 24-bit size field --
// leaves the bit order entirely up to the implementation. Shifts have none of
// those degrees of freedom, so the container code uses them exclusively.
//
// Reads and writes are bounds-checked. A truncated ROM is a file someone hands
// the tool, not a programming error, so it has to produce a message rather than
// a segfault.

#include <cstddef>
#include <span>
#include <stdexcept>
#include <string>

#include "../utils/types.hpp"

namespace ncp::rom {

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

// Rounds `value` up to the next multiple of `alignment`, which must be a power
// of two. ROM offsets are aligned in several places and getting it wrong by a
// byte moves every following region.
[[nodiscard]] constexpr u32 alignUp(u32 value, u32 alignment)
{
	return (value + (alignment - 1)) & ~(alignment - 1);
}

} // namespace ncp::rom
