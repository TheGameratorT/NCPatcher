#include "../source/build/gcc_diagnostics.hpp"

#include <iostream>

int main()
{
	bool passed = true;
	auto check = [&passed](bool condition, const char* message) {
		if (!condition)
		{
			std::cout << "FAIL: " << message << '\n';
			passed = false;
		}
	};

	const auto parsed = ncp::build::parseGccDiagnostics(R"JSON([
  {"kind":"warning","message":"unused value","option":"-Wunused-value","locations":[{"caret":{"file":"source/test.c","line":7,"display-column":11}}],"children":[
    {"kind":"note","message":"declared here","locations":[{"caret":{"file":"include/test.h","line":3,"column":4}}]}
  ]}
])JSON", "fallback.c");

	check(parsed.has_value(), "valid GCC diagnostics did not parse");
	check(parsed.has_value() && parsed->size() == 2, "child note was not flattened");
	if (parsed.has_value() && parsed->size() == 2)
	{
		check((*parsed)[0].level == ncp::msg::Level::Warning, "warning level was not preserved");
		check((*parsed)[0].message == "unused value [-Wunused-value]", "warning option was not preserved");
		check((*parsed)[0].location.file == "source/test.c", "warning file was not preserved");
		check((*parsed)[0].location.line == 7 && (*parsed)[0].location.column == 11,
			"warning line and column were not preserved");
		check((*parsed)[1].level == ncp::msg::Level::Note, "child note level was not preserved");
		check((*parsed)[1].location.file == "include/test.h", "child note file was not preserved");
	}
	check(!ncp::build::parseGccDiagnostics("not json", "fallback.c").has_value(),
		"invalid text was accepted as GCC diagnostics");
	check(!ncp::build::parseGccDiagnostics("[{}]", "fallback.c").has_value(),
		"an invalid diagnostic object suppressed the fallback");
	const auto empty = ncp::build::parseGccDiagnostics("[]", "fallback.c");
	check(empty.has_value() && empty->empty(), "an empty GCC result was not accepted");

	const std::string formatted = parsed.has_value() ? ncp::build::formatGccDiagnostics(*parsed) : "";
	check(formatted.find("source/test.c:7:11: warning: unused value") != std::string::npos,
		"human-readable formatting was not preserved");

	if (passed)
		std::cout << "gcc_diagnostics_test: all checks passed\n";
	return passed ? 0 : 1;
}
