#pragma once

#include <cstdint>

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

using uf8 = std::uint_fast8_t;
using uf16 = std::uint_fast16_t;
using uf32 = std::uint_fast32_t;
using uf64 = std::uint_fast64_t;

using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

using sf8 = std::int_fast8_t;
using sf16 = std::int_fast16_t;
using sf32 = std::int_fast32_t;
using sf64 = std::int_fast64_t;

using SizeT = std::size_t;

struct Coords { int x; int y; };
