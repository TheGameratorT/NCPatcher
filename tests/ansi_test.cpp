// Tests for source/system/ansi.{hpp,cpp}.
//
// The parser sits between every log message and every sink, so malformed input
// (which arrives for real, in compiler output piped through the build log)
// must come out unchanged rather than being swallowed or looping forever.
// Run via ctest, or directly: ./ansi_test

#include "../source/system/ansi.hpp"

#include <iostream>
#include <string>
#include <vector>

static int g_failures = 0;

static void check(bool condition, const std::string& what)
{
	if (!condition)
	{
		std::cout << "FAIL: " << what << "\n";
		g_failures++;
	}
}

// Renders the callback sequence as "text(hi)|code(m:31,1)|text(there)" so both
// the content and the ordering are asserted at once.
static std::string trace(std::string_view input)
{
	std::string out;
	Ansi::parse(input,
		[&](std::string_view run)
		{
			if (!out.empty()) out += '|';
			out += "text(" + std::string(run) + ")";
		},
		[&](char finalByte, const std::vector<int>& params)
		{
			if (!out.empty()) out += '|';
			out += "code(";
			out += finalByte;
			out += ':';
			for (std::size_t i = 0; i < params.size(); i++)
			{
				if (i) out += ',';
				out += std::to_string(params[i]);
			}
			out += ')';
		});
	return out;
}

static void testPlainText()
{
	check(trace("") == "", "empty input produces nothing");
	check(trace("hello") == "text(hello)", "text with no escapes is one run");
	check(Ansi::strip("hello") == "hello", "strip leaves plain text alone");
}

static void testSequences()
{
	check(trace("\x1b[31mred\x1b[0m") == "code(m:31)|text(red)|code(m:0)",
		"a color sequence brackets its text");
	check(trace("\x1b[31;1mx") == "code(m:31,1)|text(x)",
		"semicolon-separated parameters");
	check(trace("\x1b[mx") == "code(m:0)|text(x)",
		"an omitted parameter reads as 0");
	check(trace("\x1b[?25l") == "code(l:25)",
		"a private-use sequence is recognized, marker and all");
	check(Ansi::strip("a\x1b[?25lb") == "ab",
		"a private-use sequence leaves no residue in stripped output");
	check(trace("a\x1b[1mb\x1b[0mc") == "text(a)|code(m:1)|text(b)|code(m:0)|text(c)",
		"runs and sequences interleave in order");
}

// Ordering is the contract the Windows console sink depends on: it paints
// attributes as it goes, so text emitted before a code must not carry it.
static void testTextBeforeCodeIsEmittedFirst()
{
	check(trace("plain\x1b[31mred") == "text(plain)|code(m:31)|text(red)",
		"text preceding a sequence is emitted before the sequence");
}

static void testStripRemovesEverything()
{
	check(Ansi::strip("\x1b[31mred\x1b[0m and \x1b[1mbold\x1b[0m") == "red and bold",
		"strip removes sequences and keeps the text");
	check(Ansi::strip("\x1b[0m").empty(), "a sequence alone strips to nothing");
}

// Each of these used to be a way to lose output or spin: an ESC with no '[',
// an introducer at the very end, a sequence that never terminates.
static void testMalformedInputSurvives()
{
	// Explicit concatenation throughout: "\x1bb" would be read as one hex escape.
	check(trace("a\x1b" "b") == "text(a\x1b" "b)",
		"an ESC not followed by '[' stays in the text");
	check(trace("a\x1b") == "text(a\x1b)",
		"a trailing ESC stays in the text");
	check(trace("a\x1b[") == "text(a)",
		"an introducer with nothing after it consumes no text");
	check(trace("a\x1b[31") == "text(a)",
		"a sequence with no final byte is not reported as a code");
	check(trace("a\x1b[31\x01" "b") == "text(a)|text(\x01" "b)",
		"a byte that cannot terminate a sequence resumes literal text");
	check(Ansi::strip("\x1b\x1b\x1b") == "\x1b\x1b\x1b",
		"consecutive stray ESCs do not loop");
}

// A parameter too long to fit an int must not throw out of a logging call.
static void testOversizedParameter()
{
	check(trace("\x1b[99999999999mx") == "code(m:-1)|text(x)",
		"an unparseable parameter reads as -1 rather than throwing");
	check(Ansi::strip("\x1b[99999999999mx") == "x",
		"strip survives an unparseable parameter");
}

int main()
{
	testPlainText();
	testSequences();
	testTextBeforeCodeIsEmittedFirst();
	testStripRemovesEverything();
	testMalformedInputSurvives();
	testOversizedParameter();

	if (g_failures == 0)
	{
		std::cout << "All ANSI tests passed.\n";
		return 0;
	}
	std::cout << g_failures << " ANSI test(s) failed.\n";
	return 1;
}
