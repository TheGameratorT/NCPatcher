#pragma once

#include <filesystem>

namespace Main {

const std::filesystem::path& getAppPath();
const std::filesystem::path& getWorkPath();
const std::filesystem::path& getRomPath();
void setErrorContext(const char* errorContext);

}
