#pragma once

#include "../utils/types.hpp"
#include "../ndsbin/armbin.hpp"

namespace ncp::patch {

namespace ArenaLoFinder {

void findArenaLo(ArmBin* arm, int& arenaLoOut, u32& newcodeDestOut);

}

} // namespace ncp::patch
