#pragma once

// FNV-1a, 64-bit.
//
// Used to tell whether a build's configuration is the one the objects on disk
// were compiled under. That is a change-detection question, not a security
// one (nobody is trying to forge a rebuild), so a short non-cryptographic
// hash with a stable, obvious definition is the right tool. It has no external
// dependency and produces the same digest on every platform, which matters
// because the digest is written into a file that is shared through git.

#include <string>
#include <string_view>

#include "types.hpp"

namespace Hash {

constexpr u64 FNV_OFFSET = 14695981039346656037ull;
constexpr u64 FNV_PRIME = 1099511628211ull;

[[nodiscard]] constexpr u64 fnv1a(std::string_view text, u64 seed = FNV_OFFSET) noexcept
{
	u64 hash = seed;
	for (char c : text)
	{
		hash ^= u64(static_cast<unsigned char>(c));
		hash *= FNV_PRIME;
	}
	return hash;
}

// 16 lowercase hex digits, zero-padded, so digests sort and compare as text.
[[nodiscard]] inline std::string toHex(u64 value)
{
	static const char digits[] = "0123456789abcdef";
	std::string out(16, '0');
	for (int i = 15; i >= 0; i--)
	{
		out[std::size_t(i)] = digits[value & 0xF];
		value >>= 4;
	}
	return out;
}

[[nodiscard]] inline std::string of(std::string_view text)
{
	return toHex(fnv1a(text));
}

} // namespace Hash
