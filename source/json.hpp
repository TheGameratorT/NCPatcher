#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <rapidjson/document.h>

#include "except.hpp"

class JsonMember
{
public:
	JsonMember() {};
	JsonMember(const rapidjson::Value& value, const JsonMember* parent, const std::string& name);

    const JsonMember operator[](const char* member) const;
	const JsonMember operator[](int index) const;

	int getInt() const;
	bool getBool() const;
	const char* getString() const;

	const std::vector<JsonMember> getObjectArray() const;

	const std::vector<JsonMember> getMembers() const;
	const std::string& getName() const;
	size_t size() const;

	bool hasMember(const char* member) const;
	bool isArray() const;
	bool isObject() const;
	void assertMember(const char* member) const;
	void assertArray() const;
	void assertObject() const;
	std::string getPathToSelf() const;

private:
	const rapidjson::Value* value;
	const JsonMember* parent;
	std::string name;
};

class JsonReader
{
public:
	JsonReader(const std::filesystem::path& path);
    const JsonMember operator[](const char* member) const;
	const std::vector<JsonMember> getMembers() const;
	bool hasMember(const char* member) const;

private:
	JsonMember root;
	rapidjson::Document doc;
};
