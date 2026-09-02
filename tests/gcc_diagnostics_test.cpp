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

	// The same diagnostic as above, in the format GCC 16 emits: `json` is gone
	// from it, and this is what the compiler answers instead. Trimmed to the
	// parts that are read, and kept in GCC's own spelling -- `relatedLocations`
	// carrying a message are the notes that `children` used to hold.
	const auto sarif = ncp::build::parseGccDiagnostics(R"JSON({
  "version": "2.1.0",
  "runs": [{"results": [
    {"ruleId": "-Wunused-value", "level": "warning",
     "message": {"text": "unused value"},
     "locations": [{"physicalLocation": {
       "artifactLocation": {"uri": "source/test.c"},
       "region": {"startLine": 7, "startColumn": 11, "endColumn": 20}}}],
     "relatedLocations": [
       {"physicalLocation": {
          "artifactLocation": {"uri": "include/test.h"},
          "region": {"startLine": 3, "startColumn": 4}},
        "message": {"text": "declared here"},
        "properties": {"nestingLevel": 1}},
       {"physicalLocation": {
          "artifactLocation": {"uri": "source/test.c"},
          "region": {"startLine": 7, "startColumn": 11}}}
     ]}
  ]}]
})JSON", "fallback.c", ncp::build::DiagnosticsFormat::Sarif);

	check(sarif.has_value(), "valid SARIF diagnostics did not parse");
	check(sarif.has_value() && sarif->size() == 2,
		"a related location with no message was taken for a note");
	if (sarif.has_value() && sarif->size() == 2)
	{
		check((*sarif)[0].level == ncp::msg::Level::Warning, "SARIF warning level was not preserved");
		check((*sarif)[0].message == "unused value [-Wunused-value]",
			"a SARIF ruleId naming a warning switch was not appended");
		check((*sarif)[0].location.file == "source/test.c", "SARIF warning file was not preserved");
		check((*sarif)[0].location.line == 7 && (*sarif)[0].location.column == 11,
			"SARIF line and column were not preserved");
		check((*sarif)[1].level == ncp::msg::Level::Note, "a related location is a note");
		check((*sarif)[1].message == "declared here", "SARIF note message was not preserved");
		check((*sarif)[1].location.file == "include/test.h", "SARIF note file was not preserved");
	}

	// An ordinary error's ruleId is the level word, not a switch, and appending
	// "[error]" to every error message would be noise.
	const auto plain = ncp::build::parseGccDiagnostics(
		R"JSON({"runs":[{"results":[{"ruleId":"error","level":"error","message":{"text":"boom"}}]}]})JSON",
		"fallback.c", ncp::build::DiagnosticsFormat::Sarif);
	check(plain.has_value() && plain->size() == 1 && (*plain)[0].message == "boom",
		"a ruleId that is not a switch was appended anyway");
	check(plain.has_value() && plain->size() == 1 && (*plain)[0].location.file == "fallback.c",
		"a result with no location did not fall back to the source file");

	// Each format only reads its own shape, so a mismatch degrades to raw
	// output rather than to a wrong parse.
	check(!ncp::build::parseGccDiagnostics("[]", "fallback.c",
		ncp::build::DiagnosticsFormat::Sarif).has_value(),
		"a JSON document was accepted as SARIF");
	check(!ncp::build::parseGccDiagnostics(R"JSON({"runs":[]})JSON", "fallback.c").has_value(),
		"a SARIF document was accepted as JSON");
	check(!ncp::build::parseGccDiagnostics("[]", "fallback.c",
		ncp::build::DiagnosticsFormat::Text).has_value(),
		"text was parsed rather than passed through");
	check(ncp::build::diagnosticsFormatFlag(ncp::build::DiagnosticsFormat::Text).empty(),
		"text asked the compiler for a format");

	if (passed)
		std::cout << "gcc_diagnostics_test: all checks passed\n";
	return passed ? 0 : 1;
}
