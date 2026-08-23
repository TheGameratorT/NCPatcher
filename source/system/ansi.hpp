#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

// Minimal CSI escape-sequence parser.
//
// It exists so the console and file sinks can disagree about what to do with
// styling without each re-implementing the scan: one renders the codes, one
// drops them, and both walk the text the same way.
namespace Ansi {

// Splits text into literal runs and CSI sequences, in order.
//
//   onText  a run of literal characters (never empty)
//   onCode  a sequence: its final byte, and its numeric parameters. An omitted
//           parameter reads as 0; an unparseable one as -1.
//
// Anything that is not a well-formed CSI introducer is literal text, so
// truncated or malformed input comes out unchanged rather than disappearing.
void parse(std::string_view text,
	const std::function<void(std::string_view)>& onText,
	const std::function<void(char, const std::vector<int>&)>& onCode);

// text with every CSI sequence removed.
[[nodiscard]] std::string strip(std::string_view text);

} // namespace Ansi
