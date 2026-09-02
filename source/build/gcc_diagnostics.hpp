#pragma once

#include <filesystem>
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

// How the compiler was asked to report its diagnostics, which decides how they
// are read back.
//
// GCC has had three answers over its lifetime. `json` arrived in GCC 9 and was
// removed in GCC 16; `sarif-stderr` arrived in GCC 13 and is the one that has a
// future, being an OASIS standard rather than a GCC invention. So there is no
// single flag that works everywhere, and picking the newest first is what keeps
// this working across the range of toolchains a DS project is built with.
enum class DiagnosticsFormat
{
	Text,   // whatever the compiler prints for a human; not parsed
	Sarif,
	Json
};

// Asks the compiler which of the structured formats it accepts, preferring
// SARIF. Cached per compiler, since the answer is a property of the binary.
//
// It has to be asked rather than inferred: GCC validates the argument only when
// it compiles something, so a version probe would be guessing and a `--version`
// probe would pass for a format the compiler will later refuse. `probeDir` is
// where the empty translation unit used to ask goes.
[[nodiscard]] DiagnosticsFormat detectDiagnosticsFormat(
	const std::string& compiler,
	const std::filesystem::path& probeDir
);

// The flag that selects `format`, including a leading space, or an empty string
// for Text.
[[nodiscard]] std::string diagnosticsFormatFlag(DiagnosticsFormat format);

// Nothing when the output is not in `format` at all, which is how a compiler
// that printed something unexpected keeps its message instead of losing it.
[[nodiscard]] std::optional<std::vector<GccDiagnostic>> parseGccDiagnostics(
	std::string_view output,
	std::string_view fallbackFile,
	DiagnosticsFormat format = DiagnosticsFormat::Json
);

[[nodiscard]] std::string formatGccDiagnostics(const std::vector<GccDiagnostic>& diagnostics);

} // namespace ncp::build
