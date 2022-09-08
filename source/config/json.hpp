#pragma once

#include <string>
#include <vector>
#include <filesystem>

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
#include <rapidjson/document.h>
#ifdef __GNUC__
#pragma
#pragma GCC diagnostic pop
#endif

class JsonMember
{
public:
	explicit JsonMember();
	explicit JsonMember(const rapidjson::Value& value, const JsonMember* parent, std::string name);

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
	const rapidjson::Value* value;
	const JsonMember* parent;
	std::string name;
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
