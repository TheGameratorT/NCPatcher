#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace ncp::build {

// One in-flight compile, as the live block wants to show it. The caller is
// responsible for ordering `running` (longest-running first, to match Bazel);
// this only renders what it is given.
struct RunningAction
{
	std::string_view verb; // "Compiling" or "Assembling"
	std::string item;      // the source path
	std::chrono::seconds elapsed;
};

struct ProgressState
{
	std::size_t completed = 0;
	std::size_t total = 0;
	std::vector<RunningAction> running;
};

// Renders the live block's lines for the given state: a header
// "[done / total] N actions running" followed by up to `maxActionLines` lines,
// one per running action, each clipped to `width` columns. Beyond that cap the
// remainder collapses into a single "... N more" line.
//
// Returned lines carry ANSI styling; strip it with Ansi::strip() to measure or
// compare the visible text.
[[nodiscard]] std::vector<std::string> renderProgressBlock(
	const ProgressState& state, int width, std::size_t maxActionLines);

} // namespace ncp::build
