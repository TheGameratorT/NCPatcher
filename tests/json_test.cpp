// Tests for source/utils/json.{hpp,cpp}.
//
// Three outputs go through this writer -- the rebuild record, `config dump` and
// the --message-format json event stream -- and two of them are parsed by other
// programs. A missing comma or an unescaped quote there is not a cosmetic bug:
// it is a caller that cannot read the build's result at all.
// Run via ctest, or directly: ./json_test

#include "../source/utils/json.hpp"

#include <iostream>
#include <sstream>
#include <string>

static int g_failures = 0;

static void check(bool condition, const std::string& what)
{
	if (!condition)
	{
		std::cout << "FAIL: " << what << "\n";
		g_failures++;
	}
}

static void checkEqual(const std::string& got, const std::string& expected, const std::string& what)
{
	if (got != expected)
	{
		std::cout << "FAIL: " << what << "\n      expected: " << expected << "\n      got:      " << got << "\n";
		g_failures++;
	}
}

static void testEscaping()
{
	checkEqual(Json::escape("plain"), "plain", "text with nothing special is unchanged");
	checkEqual(Json::escape("say \"hi\""), "say \\\"hi\\\"", "quotes are escaped");
	checkEqual(Json::escape("C:\\src"), "C:\\\\src", "a Windows path's backslashes are escaped");
	checkEqual(Json::escape("a\nb\tc"), "a\\nb\\tc", "newline and tab use their short forms");
	checkEqual(Json::escape(std::string("\x01")), "\\u0001", "an unprintable control byte is escaped numerically");

	// The event stream carries compiler output, which is UTF-8 and must survive
	// intact rather than being re-encoded into escapes.
	checkEqual(Json::escape("caf\xc3\xa9"), "caf\xc3\xa9", "UTF-8 passes through untouched");
}

static std::string writeCompact(void (*body)(Json::Writer&))
{
	std::ostringstream out;
	Json::Writer writer(out);
	body(writer);
	return out.str();
}

static void testCompactShapes()
{
	checkEqual(writeCompact([](Json::Writer& w){ w.beginObject().endObject(); }),
		"{}", "an empty object");
	checkEqual(writeCompact([](Json::Writer& w){ w.beginArray().endArray(); }),
		"[]", "an empty array");

	checkEqual(writeCompact([](Json::Writer& w){
		w.beginObject().field("a", 1).field("b", "two").field("c", true).endObject();
	}), "{\"a\":1,\"b\":\"two\",\"c\":true}", "fields are comma separated");

	checkEqual(writeCompact([](Json::Writer& w){
		w.beginArray().value(1).value(2).value(3).endArray();
	}), "[1,2,3]", "array elements are comma separated");

	checkEqual(writeCompact([](Json::Writer& w){
		w.beginObject().key("empty").beginArray().endArray().field("after", 1).endObject();
	}), "{\"empty\":[],\"after\":1}", "an empty nested array does not swallow the next comma");

	checkEqual(writeCompact([](Json::Writer& w){
		w.beginObject().key("list").beginArray()
			.beginObject().field("id", 1).endObject()
			.beginObject().field("id", 2).endObject()
		.endArray().endObject();
	}), "{\"list\":[{\"id\":1},{\"id\":2}]}", "objects nested in an array");

	checkEqual(writeCompact([](Json::Writer& w){
		w.beginObject().key("x").null().endObject();
	}), "{\"x\":null}", "a null value");
}

static void testHex()
{
	checkEqual(writeCompact([](Json::Writer& w){ w.beginObject().key("a").hex(0x2065F10).endObject(); }),
		"{\"a\":\"0x02065F10\"}", "an address keeps its leading zero");
	checkEqual(writeCompact([](Json::Writer& w){ w.beginObject().key("a").hex(0x1F, 1).endObject(); }),
		"{\"a\":\"0x1F\"}", "a size is not padded");
}

// The event stream's contract is one object per line, so nothing the compact
// writer emits may contain a newline of its own.
static void testCompactStaysOnOneLine()
{
	const std::string text = writeCompact([](Json::Writer& w){
		w.beginObject().field("a", 1).key("b").beginArray().value("x").beginObject().field("c", 2).endObject().endArray().endObject();
	});
	check(text.find('\n') == std::string::npos, "compact output contains no newline");
}

static void testPrettyIsStillValid()
{
	std::ostringstream out;
	Json::Writer writer(out, 2);
	writer.beginObject();
	writer.field("name", "value");
	writer.key("list").beginArray().value(1).value(2).endArray();
	writer.key("nested").beginObject().field("deep", true).endObject();
	writer.endObject();

	const std::string text = out.str();
	checkEqual(text,
		"{\n"
		"  \"name\": \"value\",\n"
		"  \"list\": [\n"
		"    1,\n"
		"    2\n"
		"  ],\n"
		"  \"nested\": {\n"
		"    \"deep\": true\n"
		"  }\n"
		"}",
		"pretty output indents by nesting depth");
}

static void testFieldList()
{
	checkEqual(writeCompact([](Json::Writer& w){
		w.beginObject().field("flags", std::vector<std::string>{ "-Os", "-g" }).endObject();
	}), "{\"flags\":[\"-Os\",\"-g\"]}", "a string list field");

	checkEqual(writeCompact([](Json::Writer& w){
		w.beginObject().field("flags", std::vector<std::string>{}).endObject();
	}), "{\"flags\":[]}", "an empty string list field");
}

int main()
{
	testEscaping();
	testCompactShapes();
	testHex();
	testCompactStaysOnOneLine();
	testPrettyIsStillValid();
	testFieldList();

	if (g_failures == 0)
	{
		std::cout << "All JSON tests passed.\n";
		return 0;
	}
	std::cout << g_failures << " JSON test(s) failed.\n";
	return 1;
}
