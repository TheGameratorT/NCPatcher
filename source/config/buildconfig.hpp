#pragma once

#include <string>
#include <vector>
#include <filesystem>

namespace BuildConfig {

void load();

const std::string& getVariable(const std::string& value);

const std::filesystem::path& getBackupDir();
const std::filesystem::path& getFilesystemDir();
const std::string& getToolchain();

bool getBuildArm7();
const std::filesystem::path& getArm7Target();
const std::filesystem::path& getArm7BuildDir();

bool getBuildArm9();
const std::filesystem::path& getArm9Target();
const std::filesystem::path& getArm9BuildDir();

const std::vector<std::string>& getPreBuildCmds();
const std::vector<std::string>& getPostBuildCmds();

int getThreadCount();
std::time_t getLastWriteTime();

}
