#pragma once

// cfg::Document / cfg::Node, the reader every configuration file goes through,
// v1 JSON and v2 YAML alike. JSON is a subset of YAML 1.2, so one parser serves
// both and the v1 path gets the better diagnostics for free.
//
// This keeps the part of the JsonReader/JsonMember design it replaces that
// earned its keep (a node remembers the path that reached it, so an error can
// name the key it is about) and adds the part that design could not have: a
// line and column, because yaml-cpp records a Mark for every node. "Invalid
// mode" becomes "ncpatcher.yaml:88:9, in targets.arm9.regions[12].mode".
//
// Two yaml-cpp footguns this exists to contain, both silent:
//   * the non-const operator[] *inserts* the key it fails to find, so probing
//     for an optional setting mutates the document. Only const overloads are
//     reached from here.
//   * `YAML::Node a = b["x"]` aliases rather than copies, so assigning through
//     one handle rewrites the other. Nodes are held by value and never assigned
//     into after construction.

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The full header, not just node/node.h: YAML::Node's constructors and
// assignment are defined in node/impl.h, and holding one by value needs them.
#include <yaml-cpp/yaml.h>

#include "../system/except.hpp"
#include "../utils/types.hpp"

namespace cfg {

// A position in a configuration file, 1-based for display. Not every node has
// one (a node that was never in the file has nothing to point at), so the
// validity flag is checked before rendering rather than assumed.
struct Mark
{
	int line = 0;
	int column = 0;
	bool valid = false;
};

class Document;

// A configuration error that knows where it happened.
//
// what() renders the whole thing, source line and caret included, because that
// is what the existing error path prints. The structured fields are kept
// alongside it so a machine-readable output mode can emit the same failure
// without parsing English back out of the message.
class cfg_error : public ncp::exception
{
public:
	cfg_error(const Document& doc, Mark mark, std::string nodePath, std::string message);

	[[nodiscard]] const std::filesystem::path& file() const { return m_file; }
	[[nodiscard]] const Mark& mark() const { return m_mark; }
	[[nodiscard]] const std::string& nodePath() const { return m_nodePath; }
	[[nodiscard]] const std::string& message() const { return m_message; }

private:
	std::filesystem::path m_file;
	Mark m_mark;
	std::string m_nodePath;
	std::string m_message;
};

class Node
{
public:
	// An undefined node: what a lookup that found nothing returns.
	Node() = default;

	[[nodiscard]] bool defined() const;
	[[nodiscard]] bool isNull() const;
	[[nodiscard]] bool isScalar() const;
	[[nodiscard]] bool isSequence() const;
	[[nodiscard]] bool isMap() const;

	// True when the key is present and holds something other than null, which is
	// the question readers actually mean by "was this configured".
	[[nodiscard]] bool has(std::string_view key) const;

	// Missing keys and out-of-range indices come back undefined rather than
	// throwing, so a reader can ask before it demands.
	[[nodiscard]] Node operator[](std::string_view key) const;
	[[nodiscard]] Node operator[](std::size_t index) const;

	// The same lookup for a key the caller cannot proceed without.
	[[nodiscard]] Node require(std::string_view key) const;

	[[nodiscard]] std::size_t size() const;

	// Sequence entries, and map entries in document order. Order is preserved
	// because merge policy and define precedence both depend on it.
	[[nodiscard]] std::vector<Node> items() const;
	[[nodiscard]] std::vector<std::pair<std::string, Node>> fields() const;

	[[nodiscard]] std::string asString() const;
	[[nodiscard]] bool asBool() const;
	[[nodiscard]] int asInt() const;

	// Addresses and sizes reach 0x80000000 and above, where the old getInt()
	// returning int was undefined behavior rather than a diagnostic.
	[[nodiscard]] u32 asU32() const;

	// Value if present and non-null, fallback otherwise.
	[[nodiscard]] std::string asString(std::string_view fallback) const;
	[[nodiscard]] bool asBool(bool fallback) const;
	[[nodiscard]] u32 asU32(u32 fallback) const;

	[[nodiscard]] const std::string& path() const { return m_path; }
	[[nodiscard]] const Mark& mark() const { return m_mark; }

	// "ncpatcher.yaml:88:9, in targets.arm9.regions[12]", the same "where"
	// cfg_error prints, as a plain string. Diagnostics that outlive the
	// document, or that report many problems at once rather than throwing at
	// the first, keep this instead of the node.
	[[nodiscard]] std::string location() const;
	[[nodiscard]] const Document& document() const;

	// The underlying handle, for the few places that need yaml-cpp directly
	// (emitting during migration). Const, so the insert-on-lookup trap is out
	// of reach here too.
	[[nodiscard]] const YAML::Node& yaml() const { return m_node; }

	[[noreturn]] void fail(std::string message) const;

	// "Invalid type for <path>, expected <what>.", the single most common
	// config error, so it gets one spelling everywhere.
	[[noreturn]] void failType(std::string_view expected) const;

private:
	friend class Document;
	Node(const Document& doc, YAML::Node node, std::string path, Mark mark);

	[[nodiscard]] std::string childPath(std::string_view child) const;
	[[nodiscard]] std::string indexPath(std::size_t index) const;

	const Document* m_doc = nullptr;
	YAML::Node m_node;
	std::string m_path;
	Mark m_mark;

	// Tracked here rather than asked of m_node: every yaml-cpp accessor except
	// IsDefined() throws on a node that was never found, so nothing may reach
	// the handle before this has been checked.
	bool m_defined = false;
};

class Document
{
public:
	// Reads and parses `path`. Throws cfg_error on a syntax error, positioned.
	explicit Document(const std::filesystem::path& path);

	// Parses text that is not (or not yet) a file. `name` is what diagnostics
	// call it.
	Document(std::string text, std::filesystem::path name);

	// Nodes point back at the document that produced them, so it must not move
	// out from under them.
	Document(const Document&) = delete;
	Document& operator=(const Document&) = delete;
	Document(Document&&) = delete;
	Document& operator=(Document&&) = delete;

	[[nodiscard]] Node root() const;
	[[nodiscard]] const std::filesystem::path& path() const { return m_path; }

	// One line of the source text, 1-based, for caret rendering. Empty when the
	// line does not exist.
	[[nodiscard]] std::string_view line(int lineNumber) const;

private:
	void parse();

	std::filesystem::path m_path;
	std::string m_text;
	YAML::Node m_root;
};

} // namespace cfg
