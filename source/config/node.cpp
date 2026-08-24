#include "node.hpp"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <utility>

#include <yaml-cpp/yaml.h>

#include "../system/log.hpp"

namespace fs = std::filesystem;

namespace cfg {

namespace {

// A tab is one column as far as the parser is concerned but four as far as the
// reader is concerned, so the caret has to be placed against the rendered line
// rather than the raw one.
constexpr int TAB_WIDTH = 4;

std::string expandTabs(std::string_view text, int markColumn, int& caretColumnOut)
{
	std::string out;
	out.reserve(text.size());
	caretColumnOut = 0;

	for (std::size_t i = 0; i < text.size(); i++)
	{
		if (int(i) == markColumn)
			caretColumnOut = int(out.size());

		if (text[i] == '\t')
			out.append(std::size_t(TAB_WIDTH - int(out.size()) % TAB_WIDTH), ' ');
		else
			out += text[i];
	}

	// A mark one past the end of the line -- which is where "expected a value"
	// errors land -- points at the position after the last character.
	if (markColumn >= int(text.size()))
		caretColumnOut = int(out.size());

	return out;
}

Mark fromYaml(const YAML::Mark& mark)
{
	if (mark.is_null())
		return {};
	return { mark.line + 1, mark.column + 1, true };
}

bool parseUnsigned(std::string_view text, unsigned long long& out)
{
	while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
		text.remove_prefix(1);
	while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
		text.remove_suffix(1);
	if (text.empty() || text.front() == '-' || text.front() == '+')
		return false;

	int base = 10;
	if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
	{
		base = 16;
		text.remove_prefix(2);
	}

	const std::string owned(text);
	errno = 0;
	char* end = nullptr;
	const unsigned long long value = std::strtoull(owned.c_str(), &end, base);
	if (errno == ERANGE || end != owned.c_str() + owned.size())
		return false;

	out = value;
	return true;
}

bool parseSigned(std::string_view text, long long& out)
{
	const bool negative = !text.empty() && text.front() == '-';
	if (negative)
		text.remove_prefix(1);

	unsigned long long magnitude = 0;
	if (!parseUnsigned(text, magnitude))
		return false;
	if (magnitude > (negative ? 0x8000000000000000ull : 0x7FFFFFFFFFFFFFFFull))
		return false;

	out = negative ? -static_cast<long long>(magnitude) : static_cast<long long>(magnitude);
	return true;
}

} // namespace

// cfg_error =============================

cfg_error::cfg_error(const Document& doc, Mark mark, std::string nodePath, std::string message) :
	m_file(doc.path()),
	m_mark(mark),
	m_nodePath(std::move(nodePath)),
	m_message(std::move(message))
{
	std::ostringstream oss;
	oss << m_message;

	std::ostringstream where;
	where << m_file.filename().string();
	if (m_mark.valid)
		where << ':' << m_mark.line << ':' << m_mark.column;

	oss << OREASONNL "at " << OSTR(where.str());
	if (!m_nodePath.empty())
		oss << ", in " << OSTR(m_nodePath);

	if (m_mark.valid)
	{
		const std::string_view source = doc.line(m_mark.line);
		if (!source.empty())
		{
			int caret = 0;
			const std::string rendered = expandTabs(source, m_mark.column - 1, caret);
			const std::string gutter = std::to_string(m_mark.line);

			oss << OREASONNL ANSI_bBLACK << gutter << " | " ANSI_RESET << rendered;
			oss << OREASONNL ANSI_bBLACK << std::string(gutter.size(), ' ') << " | " ANSI_RESET
			    << std::string(std::size_t(caret), ' ') << ANSI_bRED "^" ANSI_RESET;
		}
	}

	m_msg = oss.str();
}

// Node =============================


Node::Node(const Document& doc, YAML::Node node, std::string path, Mark mark) :
	m_doc(&doc),
	m_node(std::move(node)),
	m_path(std::move(path)),
	m_mark(mark),
	m_defined(true)
{}

const Document& Node::document() const
{
	return *m_doc;
}

std::string Node::location() const
{
	std::ostringstream oss;
	if (m_doc != nullptr)
		oss << m_doc->path().filename().string();
	if (m_mark.valid)
		oss << ':' << m_mark.line << ':' << m_mark.column;
	if (!m_path.empty())
		oss << ", in " << m_path;
	return oss.str();
}

bool Node::defined() const { return m_defined; }
bool Node::isNull() const { return m_defined && m_node.IsNull(); }
bool Node::isScalar() const { return m_defined && m_node.IsScalar(); }
bool Node::isSequence() const { return m_defined && m_node.IsSequence(); }
bool Node::isMap() const { return m_defined && m_node.IsMap(); }

bool Node::has(std::string_view key) const
{
	const Node child = (*this)[key];
	return child.defined() && !child.isNull();
}

std::string Node::childPath(std::string_view child) const
{
	if (m_path.empty())
		return std::string(child);
	std::string out = m_path;
	out += '.';
	out += child;
	return out;
}

std::string Node::indexPath(std::size_t index) const
{
	return m_path + '[' + std::to_string(index) + ']';
}

Node Node::operator[](std::string_view key) const
{
	// A lookup into anything that is not a mapping is not an error here: the
	// reader that demanded a mapping is the one that can say what it wanted.
	if (!isMap())
		return {};

	const YAML::Node& self = m_node;
	const YAML::Node child = self[std::string(key)];
	if (!child.IsDefined())
		return {};

	return { *m_doc, child, childPath(key), fromYaml(child.Mark()) };
}

Node Node::operator[](std::size_t index) const
{
	if (!isSequence() || index >= m_node.size())
		return {};

	const YAML::Node& self = m_node;
	const YAML::Node child = self[index];
	if (!child.IsDefined())
		return {};

	return { *m_doc, child, indexPath(index), fromYaml(child.Mark()) };
}

Node Node::require(std::string_view key) const
{
	Node child = (*this)[key];
	if (!child.defined())
	{
		std::ostringstream oss;
		oss << OSTR(childPath(key)) << " was not found.";
		throw cfg_error(*m_doc, m_mark, m_path, oss.str());
	}
	return child;
}

std::size_t Node::size() const
{
	if (!m_defined || m_node.IsNull())
		return 0;
	return m_node.size();
}

std::vector<Node> Node::items() const
{
	if (!isSequence())
		failType("a list");

	std::vector<Node> out;
	out.reserve(m_node.size());

	const YAML::Node& self = m_node;
	for (std::size_t i = 0; i < self.size(); i++)
	{
		const YAML::Node child = self[i];
		out.emplace_back(Node(*m_doc, child, indexPath(i), fromYaml(child.Mark())));
	}

	return out;
}

std::vector<std::pair<std::string, Node>> Node::fields() const
{
	if (!isMap())
		failType("a mapping");

	std::vector<std::pair<std::string, Node>> out;
	out.reserve(m_node.size());

	const YAML::Node& self = m_node;
	for (auto it = self.begin(); it != self.end(); ++it)
	{
		// Held by value. Dereferencing a yaml-cpp map iterator yields a
		// temporary pair, so binding a reference into it leaves that reference
		// dangling as soon as the statement ends.
		const auto entry = *it;
		std::string key = entry.first.Scalar();
		out.emplace_back(key, Node(*m_doc, entry.second, childPath(key), fromYaml(entry.second.Mark())));
	}

	return out;
}

std::string Node::asString() const
{
	if (!isScalar())
		failType("a string");
	return m_node.Scalar();
}

bool Node::asBool() const
{
	if (!isScalar())
		failType("a boolean");

	std::string text = m_node.Scalar();
	for (char& c : text)
		c = char(std::tolower(static_cast<unsigned char>(c)));

	if (text == "true" || text == "yes" || text == "on" || text == "1")
		return true;
	if (text == "false" || text == "no" || text == "off" || text == "0")
		return false;

	failType("a boolean");
}

int Node::asInt() const
{
	if (!isScalar())
		failType("an integer");

	long long value = 0;
	if (!parseSigned(m_node.Scalar(), value) || value < INT32_MIN || value > INT32_MAX)
		failType("an integer");

	return int(value);
}

u32 Node::asU32() const
{
	if (!isScalar())
		failType("an integer or hexadecimal string");

	unsigned long long value = 0;
	if (!parseUnsigned(m_node.Scalar(), value) || value > 0xFFFFFFFFull)
		failType("an integer or hexadecimal string");

	return u32(value);
}

std::string Node::asString(std::string_view fallback) const
{
	if (!m_defined || isNull())
		return std::string(fallback);
	return asString();
}

bool Node::asBool(bool fallback) const
{
	if (!m_defined || isNull())
		return fallback;
	return asBool();
}

u32 Node::asU32(u32 fallback) const
{
	if (!m_defined || isNull())
		return fallback;
	return asU32();
}

void Node::fail(std::string message) const
{
	throw cfg_error(*m_doc, m_mark, m_path, std::move(message));
}

void Node::failType(std::string_view expected) const
{
	std::ostringstream oss;
	oss << "Invalid type for " << OSTR(m_path) << ", expected " ANSI_bCYAN << expected << ANSI_RESET ".";
	fail(oss.str());
}

// Document =============================

Document::Document(const fs::path& path) :
	m_path(path)
{
	if (!fs::exists(path))
		throw ncp::file_error(path, ncp::file_error::find);

	std::ifstream input(path, std::ios::binary);
	if (!input.is_open())
		throw ncp::file_error(path, ncp::file_error::read);

	std::ostringstream buffer;
	buffer << input.rdbuf();
	m_text = buffer.str();

	parse();
}

Document::Document(std::string text, fs::path name) :
	m_path(std::move(name)),
	m_text(std::move(text))
{
	parse();
}

void Document::parse()
{
	try {
		m_root = YAML::Load(m_text);
	} catch (const YAML::ParserException& e) {
		throw cfg_error(*this, fromYaml(e.mark), {}, e.msg + ".");
	} catch (const YAML::Exception& e) {
		throw cfg_error(*this, fromYaml(e.mark), {}, e.msg + ".");
	}
}

Node Document::root() const
{
	return { *this, m_root, {}, fromYaml(m_root.Mark()) };
}

std::string_view Document::line(int lineNumber) const
{
	if (lineNumber < 1)
		return {};

	std::size_t start = 0;
	for (int current = 1; current < lineNumber; current++)
	{
		start = m_text.find('\n', start);
		if (start == std::string::npos)
			return {};
		start++;
	}

	std::size_t end = m_text.find('\n', start);
	if (end == std::string::npos)
		end = m_text.size();
	if (end > start && m_text[end - 1] == '\r')
		end--;

	return std::string_view(m_text).substr(start, end - start);
}

} // namespace cfg
