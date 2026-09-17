#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace Log {

// A block of lines pinned below everything already printed, redrawn in place
// rather than scrolled. Meant for a build's live progress display.
//
// It is always the last thing on screen: every redraw erases from the block's
// first row to the end of the screen, so anything a caller printed underneath
// it in the meantime would be eaten. Print through Log::out first, then call
// render() or clear().
//
// Reaches only a terminal that can render escapes: both render() and clear()
// write under LogMode::Console, which PlainSink and FileSink both reject. A
// piped build or a build with --color never simply never draws anything here.
class LiveBlock
{
public:
	~LiveBlock();

	// Replaces the block with the given lines. Each is written as is; a caller
	// that wants styling or width clipping applies it beforehand.
	void render(const std::vector<std::string>& lines);

	// Erases the block, leaving the cursor where the block used to start.
	void clear();

private:
	std::size_t m_drawn = 0;
};

} // namespace Log
