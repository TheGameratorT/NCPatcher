#include "env_file.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

#include "../system/except.hpp"
#include "../system/log.hpp"

namespace fs = std::filesystem;

namespace ncp::config {

namespace {

std::string_view trim(std::string_view text)
{
	const auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
	while (!text.empty() && isSpace(text.front()))
		text.remove_prefix(1);
	while (!text.empty() && isSpace(text.back()))
		text.remove_suffix(1);
	return text;
}

bool isValidName(std::string_view name)
{
	// The same shape the hook `env:` schema accepts, so the two ways of setting
	// a variable agree on what a variable may be called.
	if (name.empty())
		return false;
	if (!(std::isalpha(static_cast<unsigned char>(name.front())) || name.front() == '_'))
		return false;
	for (const char c : name)
	{
		if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_'))
			return false;
	}
	return true;
}

// Strips one layer of matching quotes. Not shell quoting -- there is no
// escaping and no expansion inside -- just a way to write a value with leading
// or trailing spaces, or one that would otherwise be ambiguous.
std::string_view unquote(std::string_view value)
{
	if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'')
	    && value.back() == value.front())
	{
		return value.substr(1, value.size() - 2);
	}
	return value;
}

[[noreturn]] void fail(const fs::path& file, std::size_t line, const std::string& what)
{
	std::ostringstream oss;
	oss << file.string() << ":" << line << ": " << what;
	throw ncp::exception(oss.str());
}

} // namespace

void EnvFile::load(const fs::path& file)
{
	m_file = file;
	m_entries.clear();
	m_loaded = false;

	std::ifstream stream(file);
	if (!stream.is_open())
		return;

	m_loaded = true;

	std::string raw;
	std::size_t lineNumber = 0;
	while (std::getline(stream, raw))
	{
		lineNumber++;

		const std::string_view line = trim(raw);
		if (line.empty() || line.front() == '#')
			continue;

		const std::size_t separator = line.find('=');
		if (separator == std::string_view::npos)
		{
			std::ostringstream oss;
			oss << "Expected " << OSTRa("NAME=VALUE") << ", got " << OSTR(std::string(line)) << ".";
			fail(file, lineNumber, oss.str());
		}

		const std::string_view name = trim(line.substr(0, separator));

		// `export NAME=...` is the one shell-ism worth diagnosing by name: it is
		// what anyone who has written a .env before will reach for, and silently
		// creating a variable called "export NAME" would be baffling.
		if (name.starts_with("export "))
			fail(file, lineNumber, "This file is not a shell script; drop the "
			                       ANSI_bCYAN "export" ANSI_RESET " keyword.");

		if (!isValidName(name))
		{
			std::ostringstream oss;
			oss << "Invalid variable name " << OSTR(std::string(name)) << "."
			    << OREASONNL << "A name starts with a letter or underscore and continues with "
			                    "letters, digits or underscores.";
			fail(file, lineNumber, oss.str());
		}

		std::string value(unquote(trim(line.substr(separator + 1))));

		const auto existing = std::find_if(m_entries.begin(), m_entries.end(),
			[&](const auto& entry) { return entry.first == name; });
		if (existing != m_entries.end())
			existing->second = std::move(value);
		else
			m_entries.emplace_back(std::string(name), std::move(value));
	}
}

const std::string* EnvFile::find(std::string_view name) const
{
	for (const auto& [key, value] : m_entries)
	{
		if (key == name)
			return &value;
	}
	return nullptr;
}

} // namespace ncp::config
