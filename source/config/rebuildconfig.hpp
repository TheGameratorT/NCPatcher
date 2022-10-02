#pragma once

#include <string>
#include <vector>
#include <filesystem>

#include "../types.hpp"

namespace RebuildConfig {

void load();
void save();

std::time_t getBuildConfigWriteTime();
std::time_t getArm7TargetWriteTime();
std::time_t getArm9TargetWriteTime();
std::vector<u32>& getArm7PatchedOvs();
std::vector<u32>& getArm9PatchedOvs();

void setBuildConfigWriteTime(std::time_t value);
void setArm7TargetWriteTime(std::time_t value);
void setArm9TargetWriteTime(std::time_t value);

}
