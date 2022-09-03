#pragma once

#include <filesystem>

namespace Main {

const std::filesystem::path& getAppPath();
const std::filesystem::path& getWorkPath();
void setErrorContext(const char* errorContext);

}
