// Tests for source/build/progress_block.{hpp,cpp}.
//
// The renderer is kept pure (no terminal, no thread pool) exactly so it can be
// exercised here; BuildLogger only has to feed it state and hand the result to
// a LiveBlock. Run via ctest, or directly: ./progress_block_test

#include "../source/build/progress_block.hpp"

#include <iostream>
#include <string>

#include "../source/system/ansi.hpp"
#include "../source/system/log.hpp"

using namespace ncp::build;
using namespace std::chrono_literals;

static int g_failures = 0;

static void check(bool condition, const std::string& what)
{
	if (!condition)
	{
		std::cout << "FAIL: " << what << "\n";
		g_failures++;
	}
}

static RunningAction action(std::string_view verb, std::string item, std::chrono::seconds elapsed)
{
	return RunningAction{verb, std::move(item), elapsed};
}

static void testHeaderPadding()
{
	ProgressState state;
	state.completed = 7;
	state.total = 143;
	state.running = {action("Compiling", "a.cpp", 1s)};

	const auto lines = renderProgressBlock(state, 80, 8);
	check(!lines.empty(), "header line is present");
	check(Ansi::strip(lines[0]).starts_with("[   7 / 143]"),
		"the numerator is right-justified one column wider than the denominator's digits");
}

static void testActionPluralization()
{
	ProgressState one;
	one.completed = 0;
	one.total = 1;
	one.running = {action("Compiling", "a.cpp", 1s)};
	check(Ansi::strip(renderProgressBlock(one, 80, 8)[0]).ends_with("1 action running"),
		"exactly one running action is singular");

	ProgressState eight;
	eight.completed = 0;
	eight.total = 8;
	for (int i = 0; i < 8; i++)
		eight.running.push_back(action("Compiling", "a.cpp", 1s));
	check(Ansi::strip(renderProgressBlock(eight, 80, 8)[0]).ends_with("8 actions running"),
		"more than one running action is plural");

	ProgressState zero;
	zero.completed = 0;
	zero.total = 0;
	check(Ansi::strip(renderProgressBlock(zero, 80, 8)[0]).ends_with("0 actions running"),
		"zero running actions is plural");
}

static void testElapsedFormatting()
{
	ProgressState state;
	state.completed = 0;
	state.total = 2;
	state.running = {
		action("Compiling", "fresh.cpp", 0s),
		action("Compiling", "slow.cpp", 95s),
	};

	const auto lines = renderProgressBlock(state, 80, 8);
	check(Ansi::strip(lines[1]) == "    Compiling fresh.cpp",
		"an action under a second carries no elapsed suffix");
	check(Ansi::strip(lines[2]) == "    Compiling slow.cpp; 1m 35s",
		"95 seconds renders as minutes and seconds");
}

static void testTruncation()
{
	ProgressState state;
	state.completed = 0;
	state.total = 12;
	for (int i = 0; i < 12; i++)
		state.running.push_back(action("Compiling", "file" + std::to_string(i) + ".cpp", 1s));

	const auto lines = renderProgressBlock(state, 80, 8);
	// 1 header + up to 8 action-area rows: the "more" summary counts against
	// that cap, so 7 actions are shown individually and the 8th row summarizes
	// the rest.
	check(lines.size() == 9, "the block never exceeds maxActionLines + 1 rows");
	check(Ansi::strip(lines.back()) == "    ... 5 more",
		"the remainder collapses into a single summary line");
}

static void testNoTruncationWhenEverythingFits()
{
	ProgressState state;
	state.completed = 0;
	state.total = 3;
	for (int i = 0; i < 3; i++)
		state.running.push_back(action("Compiling", "file" + std::to_string(i) + ".cpp", 1s));

	const auto lines = renderProgressBlock(state, 80, 8);
	check(lines.size() == 4, "one header row plus one row per action, no summary");
}

static void testWidthClipping()
{
	ProgressState state;
	state.completed = 0;
	state.total = 1;
	state.running = {action("Compiling",
		"source/some/very/deeply/nested/directory/tree/that/does/not/fit/on/one/line.cpp", 0s)};

	const auto lines = renderProgressBlock(state, 40, 8);
	const std::string plain = Ansi::strip(lines[1]);
	check(plain.size() <= 40, "a clipped line does not exceed the requested width");
	check(plain.find("...") != std::string::npos, "an overlong path is elided");
	check(plain.starts_with("    Compiling "), "the verb and prefix survive clipping");

	// The escape sequences must never be cut in half by the width limit: the
	// raw line still ends with a full ANSI_RESET.
	check(lines[1].ends_with(ANSI_RESET), "styling is applied after clipping, never split by it");
}

int main()
{
	testHeaderPadding();
	testActionPluralization();
	testElapsedFormatting();
	testTruncation();
	testNoTruncationWhenEverythingFits();
	testWidthClipping();

	if (g_failures == 0)
	{
		std::cout << "All progress block tests passed.\n";
		return 0;
	}
	std::cout << g_failures << " progress block test(s) failed.\n";
	return 1;
}
