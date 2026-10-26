#include "patch_name.hpp"

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace ncp::patch {

static std::vector<std::string_view> splitTokens(std::string_view name)
{
	std::vector<std::string_view> tokens;
	std::size_t start = 0;
	while (true)
	{
		std::size_t end = name.find('_', start);
		if (end == std::string_view::npos)
		{
			tokens.push_back(name.substr(start));
			return tokens;
		}
		tokens.push_back(name.substr(start, end - start));
		start = end + 1;
	}
}

// Throws std::invalid_argument or std::out_of_range when `str` is not a
// decimal or 0x-prefixed hexadecimal number.
static unsigned long parseNumber(std::string_view str)
{
	std::string s(str);
	std::size_t used = 0;
	unsigned long value = s.starts_with("0x") ? std::stoul(s.substr(2), &used, 16) : std::stoul(s, &used, 10);
	if (used != (s.starts_with("0x") ? s.size() - 2 : s.size()))
		throw std::invalid_argument("trailing characters");
	return value;
}

static bool isIdentifier(std::string_view str)
{
	if (str.empty() || (str[0] >= '0' && str[0] <= '9'))
		return false;
	for (char c : str)
	{
		bool valid = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
		if (!valid)
			return false;
	}
	return true;
}

bool parsePatchName(std::string_view name, PatchNameForm form, PatchName& out, std::string& error)
{
	error.clear();
	out = PatchName();

	std::vector<std::string_view> tokens = splitTokens(name);
	if (tokens.size() < 2)
		return false;

	std::string_view typeName = tokens[0];
	PatchType type = patchTypeFromString(typeName);
	if (type == PatchType::Invalid)
	{
		error = "Found invalid patch type: " + std::string(typeName);
		return false;
	}

	// The symver ids are always the last three tokens, so dropping them leaves
	// a label name whose group may contain underscores like any other.
	std::size_t end = tokens.size();
	if (form == PatchNameForm::Symver)
		end -= std::min<std::size_t>(3, end - 2);

	try {
		out.destAddress = u32(parseNumber(tokens[1]));
	} catch (std::exception&) {
		error = "Found invalid address for patch: " + std::string(name);
		return false;
	}

	std::size_t i = 2;
	if (i < end && tokens[i].starts_with("ov"))
	{
		try {
			out.destAddressOv = int(parseNumber(tokens[i].substr(2)));
		} catch (std::exception&) {
			error = "Found invalid overlay for patch: " + std::string(name);
			return false;
		}
		i++;
	}

	if (i < end && tokens[i].starts_with("g"))
	{
		std::string group(tokens[i].substr(1));
		for (i++; i < end; i++)
		{
			group += '_';
			group += tokens[i];
		}
		if (!isIdentifier(group))
		{
			error = "Found invalid group for patch: " + std::string(name);
			return false;
		}
		out.group = std::move(group);
	}

	if (i < end)
	{
		error = "Expected an overlay or group in patch for: " + std::string(name);
		return false;
	}

	out.type = type;
	return true;
}

} // namespace ncp::patch
