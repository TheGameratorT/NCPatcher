#pragma once

// Just enough JSON to emit it.
//
// Three things in this program produce machine-readable output -- the rebuild
// record, `config dump`, and the --message-format json event stream -- and
// before this they each hand-wrote their braces. That is survivable while the
// only values are hex hashes, and stops being survivable the moment a Windows
// path or a compiler message with a quote in it goes through.
//
// There is no reader here on purpose: everything this program *reads* is YAML,
// and JSON is a subset of YAML 1.2, so cfg::Document already covers it.

#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "types.hpp"

namespace Json {

// The contents of a JSON string, without the surrounding quotes.
[[nodiscard]] std::string escape(std::string_view text);

// A streaming writer that tracks nesting, so callers write what they mean
// rather than counting commas.
//
// Two layouts: pretty, for a file a person will open, and compact, for the
// newline-delimited event stream where one event must be one line.
class Writer
{
public:
	// indentWidth 0 emits everything on a single line.
	explicit Writer(std::ostream& out, int indentWidth = 0);

	Writer& beginObject();
	Writer& endObject();
	Writer& beginArray();
	Writer& endArray();

	// Names the next value. Only valid inside an object.
	Writer& key(std::string_view name);

	Writer& value(std::string_view text);
	Writer& value(const char* text) { return value(std::string_view(text)); }
	Writer& value(const std::string& text) { return value(std::string_view(text)); }
	Writer& value(bool flag);
	Writer& value(int number);
	Writer& value(long long number);
	Writer& value(unsigned long long number);
	Writer& value(u32 number) { return value(static_cast<unsigned long long>(number)); }
	Writer& value(std::size_t number) { return value(static_cast<unsigned long long>(number)); }
	Writer& null();

	// "0x02065F10" -- an address is far more legible as one, and JSON has no
	// hexadecimal literal, so it goes out as a string.
	Writer& hex(u32 number, int minDigits = 8);

	template <typename T>
	Writer& field(std::string_view name, const T& v) { key(name); return value(v); }

	Writer& field(std::string_view name, const std::vector<std::string>& list);

private:
	void prepareValue();
	void newline();

	std::ostream& m_out;
	int m_indentWidth;
	int m_depth = 0;
	// True between an object's key and its value, where no separator or
	// indentation may be emitted.
	bool m_afterKey = false;
	// True when the innermost container has had at least one entry, i.e. when
	// the next one needs a comma in front of it.
	std::vector<bool> m_populated;
};

} // namespace Json
