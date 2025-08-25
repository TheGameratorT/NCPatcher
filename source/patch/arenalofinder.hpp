#pragma once

#include "../utils/types.hpp"
#include "../ndsbin/armbin.hpp"

namespace ArenaLoFinder {

void findArenaLo(ArmBin* arm, int& arenaLoOut, u32& newcodeDestOut);

}
