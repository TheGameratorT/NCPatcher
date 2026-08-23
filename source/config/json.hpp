#pragma once

#include <string>
#include <vector>
#include <filesystem>

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#include <rapidjson/document.h>
#ifdef __GNUC__
#pragma
#pragma GCC diagnostic pop
#endif
#ifdef __clang__
#pragma clang diagnostic pop
#endif

class JsonMember
{
public:
	explicit JsonMember();
	explicit JsonMember(const rapidjson::Value& value, std::string path, std::string name);

	JsonMember operator[](const char* member) const;
	JsonMember operator[](size_t index) const;

	[[nodiscard]] int getInt() const;
	[[nodiscard]] bool getBool() const;
	[[nodiscard]] const char* getString() const;

	[[nodiscard]] std::vector<JsonMember> getObjectArray() const;

	[[nodiscard]] std::vector<JsonMember> getMembers() const;
	[[nodiscard]] const std::string& getName() const;
	[[nodiscard]] size_t size() const;
	[[nodiscard]] size_t memberCount() const;

	[[nodiscard]] bool hasMember(const char* member) const;
	[[nodiscard]] bool isArray() const;
	[[nodiscard]] bool isObject() const;
	[[nodiscard]] bool isNull() const;
	void assertMember(const char* member) const;
	void assertArray() const;
	void assertObject() const;
	[[nodiscard]] std::string getPathToSelf() const;

private:
	// The path is stored by value rather than walked through parent pointers:
	// a JsonMember may outlive the temporary it was indexed from.
	const rapidjson::Value* value;
	std::string path;
	std::string name;

	[[nodiscard]] std::string childPath(std::string_view child) const;
};

class JsonReader
{
public:
	explicit JsonReader(const std::filesystem::path& path);
	JsonMember operator[](const char* member) const;
	[[nodiscard]] std::vector<JsonMember> getMembers() const;
	bool hasMember(const char* member) const;

private:
	JsonMember root;
	rapidjson::Document doc;
};
