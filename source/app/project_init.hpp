#pragma once

#include <filesystem>
#include <string_view>

namespace ncp::project {

void initialize(const std::filesystem::path& root, std::string_view templateName);

} // namespace ncp::project
