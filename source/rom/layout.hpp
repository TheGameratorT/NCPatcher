#pragma once

#include "../utils/types.hpp"

namespace ncp::rom {

// Rounds `value` up to the next multiple of `alignment`, which must be a power
// of two. ROM offsets are aligned in several places and getting it wrong by a
// byte moves every following region.
[[nodiscard]] constexpr u32 alignUp(u32 value, u32 alignment)
{
	return (value + (alignment - 1)) & ~(alignment - 1);
}

} // namespace ncp::rom
