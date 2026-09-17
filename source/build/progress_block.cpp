#include "progress_block.hpp"

#include <algorithm>

#include "../system/log.hpp"

namespace ncp::build {

namespace {

std::size_t digitsOf(std::size_t n)
{
	std::size_t d = 1;
	while (n >= 10)
	{
		n /= 10;
		d++;
	}
	return d;
}

// "" below one second, so a freshly started action does not flash a "0s" that
// is gone before anyone could read it; "Ns" under a minute; "Mm Ns" beyond.
std::string formatElapsed(std::chrono::seconds elapsed)
{
	const long long total = elapsed.count();
	if (total < 1)
		return {};
	if (total < 60)
		return std::to_string(total) + "s";
	return std::to_string(total / 60) + "m " + std::to_string(total % 60) + "s";
}

// Keeps the ends of `text` and drops the middle, since the basename (the end)
// is the informative half of a path. Operates on plain text; styling is
// applied by the caller after clipping, so an escape sequence is never split.
std::string clipMiddle(std::string_view text, int width)
{
	if (width <= 0)
		return {};
	if (int(text.size()) <= width)
		return std::string(text);

	static constexpr std::string_view ellipsis = "...";
	if (width <= int(ellipsis.size()))
		return std::string(ellipsis.substr(0, std::size_t(width)));

	const int keep = width - int(ellipsis.size());
	const int front = (keep + 1) / 2;
	const int back = keep - front;

	std::string out;
	out.reserve(std::size_t(width));
	out.append(text.substr(0, std::size_t(front)));
	out.append(ellipsis);
	out.append(text.substr(text.size() - std::size_t(back)));
	return out;
}

std::string renderHeader(const ProgressState& state)
{
	// Padded one wider than the denominator needs, so the header does not sit
	// flush against the bracket the moment the count reaches full digit width.
	const std::size_t numWidth = digitsOf(state.total) + 1;
	std::string numerator = std::to_string(state.completed);
	if (numerator.size() < numWidth)
		numerator.insert(0, numWidth - numerator.size(), ' ');

	std::string line;
	line.reserve(64);
	line += ANSI_bWHITE "[" ANSI_RESET;
	line += numerator;
	line += " / ";
	line += std::to_string(state.total);
	line += ANSI_bWHITE "]" ANSI_RESET;
	line += ' ';
	line += std::to_string(state.running.size());
	line += (state.running.size() == 1) ? " action running" : " actions running";
	return line;
}

std::string renderActionLine(const RunningAction& action, int width)
{
	const std::string elapsed = formatElapsed(action.elapsed);
	const std::string suffix = elapsed.empty() ? std::string() : ("; " + elapsed);

	std::string prefix;
	prefix.reserve(16);
	prefix += "    ";
	prefix += action.verb;
	prefix += ' ';

	const int itemWidth = width - int(prefix.size()) - int(suffix.size());
	const std::string item = clipMiddle(action.item, itemWidth);

	std::string line;
	line.reserve(prefix.size() + item.size() + suffix.size() + 16);
	line += prefix;
	line += ANSI_bYELLOW;
	line += item;
	line += ANSI_RESET;
	line += suffix;
	return line;
}

} // namespace

std::vector<std::string> renderProgressBlock(
	const ProgressState& state, int width, std::size_t maxActionLines)
{
	std::vector<std::string> lines;
	lines.reserve(1 + std::min(state.running.size(), maxActionLines));

	lines.push_back(renderHeader(state));

	if (maxActionLines == 0)
		return lines;

	// When truncating, the summary line itself counts against the cap, so the
	// action area never exceeds maxActionLines rows.
	const bool truncated = state.running.size() > maxActionLines;
	const std::size_t shown = truncated ? (maxActionLines - 1) : state.running.size();

	for (std::size_t i = 0; i < shown; i++)
		lines.push_back(renderActionLine(state.running[i], width));

	if (truncated)
	{
		const std::size_t more = state.running.size() - shown;
		lines.push_back("    ... " + std::to_string(more) + " more");
	}

	return lines;
}

} // namespace ncp::build
