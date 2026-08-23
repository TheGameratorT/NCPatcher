#include "ansi.hpp"

namespace Ansi {

namespace {

// CSI byte classes, per ECMA-48: parameters, then optional intermediates, then
// exactly one final byte that says what the sequence does.
bool isParamByte(char c)        { return c >= 0x30 && c <= 0x3F; }
bool isIntermediateByte(char c) { return c >= 0x20 && c <= 0x2F; }
bool isFinalByte(char c)        { return c >= 0x40 && c <= 0x7E; }

// The leading byte of a private-use sequence such as ESC [ ? 25 l, which hides
// the cursor. It is not a parameter, and reading it as one is how "?25l" used
// to end up as literal text in the log file.
bool isPrivateMarker(char c) { return c >= 0x3C && c <= 0x3F; }

// Deliberately not std::stoi: a parameter long enough to overflow is malformed
// input from a subprocess, not a reason to throw out of a logging call.
// Omitted means 0, as CSI defines; malformed means -1, which no sequence uses,
// so a sink can ignore it without mistaking it for a real code.
int parseParam(std::string_view s)
{
	int value = 0;
	for (char c : s)
	{
		if (c < '0' || c > '9' || value > 99999)
			return -1;
		value = value * 10 + (c - '0');
	}
	return value;
}

void splitParams(std::string_view text, std::vector<int>& out)
{
	out.clear();

	std::size_t start = 0;
	for (std::size_t i = 0; i <= text.size(); i++)
	{
		if (i == text.size() || text[i] == ';')
		{
			out.push_back(parseParam(text.substr(start, i - start)));
			start = i + 1;
		}
	}
}

} // namespace

void parse(std::string_view text,
	const std::function<void(std::string_view)>& onText,
	const std::function<void(char, const std::vector<int>&)>& onCode)
{
	const std::size_t length = text.length();
	std::size_t cursor = 0;   // scan position
	std::size_t literal = 0;  // start of the literal run not yet emitted

	std::vector<int> params;

	while ((cursor = text.find('\x1b', cursor)) != std::string_view::npos)
	{
		// An ESC not followed by '[' is not a CSI introducer. Step over it and
		// keep it in the literal run, or a stray ESC would spin here forever.
		if (cursor + 1 >= length || text[cursor + 1] != '[')
		{
			cursor++;
			continue;
		}

		if (cursor > literal)
			onText(text.substr(literal, cursor - literal));

		cursor += 2; // past the ESC and the '['

		if (cursor < length && isPrivateMarker(text[cursor]))
			cursor++;

		std::size_t scan = cursor;
		while (scan < length && isParamByte(text[scan]))
			scan++;

		const std::size_t paramsEnd = scan;

		while (scan < length && isIntermediateByte(text[scan]))
			scan++;

		// Ran off the end mid-sequence, or hit a byte that cannot terminate
		// one. Either way there is no code to report; drop what was scanned and
		// carry on from there rather than re-reading it as text, which would
		// put half an escape sequence in the output.
		if (scan >= length)
		{
			literal = length;
			break;
		}
		if (!isFinalByte(text[scan]))
		{
			cursor = scan;
			literal = scan;
			continue;
		}

		splitParams(text.substr(cursor, paramsEnd - cursor), params);
		onCode(text[scan], params);

		cursor = scan + 1;
		literal = cursor;
	}

	if (literal < length)
		onText(text.substr(literal));
}

std::string strip(std::string_view text)
{
	std::string out;
	out.reserve(text.size());
	parse(text,
		[&](std::string_view run) { out.append(run); },
		[](char, const std::vector<int>&) {});
	return out;
}

} // namespace Ansi
