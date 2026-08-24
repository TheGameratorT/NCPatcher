#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../system/message.hpp"

namespace ncp::build {

struct GccDiagnostic
{
	msg::Level level;
	std::string message;
	msg::Location location;
};

[[nodiscard]] std::optional<std::vector<GccDiagnostic>> parseGccDiagnostics(
	std::string_view output,
	std::string_view fallbackFile
);

[[nodiscard]] std::string formatGccDiagnostics(const std::vector<GccDiagnostic>& diagnostics);

} // namespace ncp::build
