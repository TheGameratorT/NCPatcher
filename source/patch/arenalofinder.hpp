#pragma once

#include "../types.hpp"
#include "../ndsbin/armbin.hpp"

namespace ArenaLoFinder {

void findArenaLo(ArmBin* arm, int& arenaLoOut, u32& newcodeDestOut);

}
