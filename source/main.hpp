#pragma once

#include <filesystem>
#include <vector>
#include <string>

namespace Main {

const std::filesystem::path& getAppPath();
const std::filesystem::path& getWorkPath();
const std::filesystem::path& getRomPath();
void setErrorContext(const char* errorContext);
bool getVerbose();
const std::vector<std::string>& getDefines();

}
