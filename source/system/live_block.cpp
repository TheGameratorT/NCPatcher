#include "live_block.hpp"

#include "log.hpp"

namespace Log {

LiveBlock::~LiveBlock()
{
	clear();
}

void LiveBlock::clear()
{
	if (m_drawn == 0)
		return;

	const LogMode previousMode = getMode();
	setMode(LogMode::Console);

	out << '\r';
	if (m_drawn > 1)
		out << "\x1b[" << (m_drawn - 1) << 'A';
	out << "\x1b[J" << std::flush;

	setMode(previousMode);

	m_drawn = 0;
}

void LiveBlock::render(const std::vector<std::string>& lines)
{
	clear();

	if (lines.empty())
		return;

	const LogMode previousMode = getMode();
	setMode(LogMode::Console);

	for (std::size_t i = 0; i < lines.size(); i++)
	{
		if (i > 0)
			out << '\n';
		out << lines[i];
	}
	out << std::flush;

	setMode(previousMode);

	m_drawn = lines.size();
}

} // namespace Log
