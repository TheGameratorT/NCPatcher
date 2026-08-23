#include "json.hpp"

#include <cstdio>

namespace Json {

std::string escape(std::string_view text)
{
	std::string out;
	out.reserve(text.size());

	for (unsigned char c : text)
	{
		switch (c)
		{
		case '"':  out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\b': out += "\\b"; break;
		case '\f': out += "\\f"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			// Anything above 0x1F goes through untouched, including UTF-8
			// continuation bytes: the input is already UTF-8 and JSON strings
			// are defined over Unicode, so re-encoding would only corrupt it.
			if (c < 0x20)
			{
				char buffer[7];
				std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
				out += buffer;
			}
			else
			{
				out += static_cast<char>(c);
			}
			break;
		}
	}

	return out;
}

Writer::Writer(std::ostream& out, int indentWidth) :
	m_out(out),
	m_indentWidth(indentWidth)
{}

void Writer::newline()
{
	if (m_indentWidth <= 0)
		return;
	m_out << '\n';
	for (int i = 0; i < m_depth * m_indentWidth; i++)
		m_out << ' ';
}

// Emits whatever has to come before a value: the comma separating it from the
// previous entry, and the indentation. Neither applies straight after a key.
void Writer::prepareValue()
{
	if (m_afterKey)
	{
		m_afterKey = false;
		return;
	}

	if (!m_populated.empty())
	{
		if (m_populated.back())
			m_out << ',';
		m_populated.back() = true;
		newline();
	}
}

Writer& Writer::beginObject()
{
	prepareValue();
	m_out << '{';
	m_depth++;
	m_populated.push_back(false);
	return *this;
}

Writer& Writer::endObject()
{
	const bool populated = m_populated.empty() ? false : m_populated.back();
	if (!m_populated.empty())
		m_populated.pop_back();
	m_depth--;
	if (populated)
		newline();
	m_out << '}';
	return *this;
}

Writer& Writer::beginArray()
{
	prepareValue();
	m_out << '[';
	m_depth++;
	m_populated.push_back(false);
	return *this;
}

Writer& Writer::endArray()
{
	const bool populated = m_populated.empty() ? false : m_populated.back();
	if (!m_populated.empty())
		m_populated.pop_back();
	m_depth--;
	if (populated)
		newline();
	m_out << ']';
	return *this;
}

Writer& Writer::key(std::string_view name)
{
	prepareValue();
	m_out << '"' << escape(name) << "\":";
	if (m_indentWidth > 0)
		m_out << ' ';
	m_afterKey = true;
	return *this;
}

Writer& Writer::value(std::string_view text)
{
	prepareValue();
	m_out << '"' << escape(text) << '"';
	return *this;
}

Writer& Writer::value(bool flag)
{
	prepareValue();
	m_out << (flag ? "true" : "false");
	return *this;
}

Writer& Writer::value(int number)
{
	prepareValue();
	m_out << number;
	return *this;
}

Writer& Writer::value(long long number)
{
	prepareValue();
	m_out << number;
	return *this;
}

Writer& Writer::value(unsigned long long number)
{
	prepareValue();
	m_out << number;
	return *this;
}

Writer& Writer::null()
{
	prepareValue();
	m_out << "null";
	return *this;
}

Writer& Writer::hex(u32 number, int minDigits)
{
	char buffer[16];
	std::snprintf(buffer, sizeof(buffer), "0x%0*X", minDigits, number);
	return value(std::string_view(buffer));
}

Writer& Writer::field(std::string_view name, const std::vector<std::string>& list)
{
	key(name);
	beginArray();
	for (const std::string& entry : list)
		value(entry);
	return endArray();
}

} // namespace Json
