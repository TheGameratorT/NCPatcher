#include "json.hpp"

#include <fstream>
#include <string_view>
#include <rapidjson/istreamwrapper.h>
#include <rapidjson/error/en.h>

namespace fs = std::filesystem;
namespace rj = rapidjson;

// JsonNode

JsonMember::JsonMember(const rj::Value& value, const JsonMember* parent, const std::string& name) :
	value(&value),
	parent(parent),
	name(name)
{}

const JsonMember JsonMember::operator[](const char* member) const
{
	assertMember(member);
	return JsonMember((*value)[member], this, member);
}

const JsonMember JsonMember::operator[](int index) const
{
	assertArray();
	std::string sindex = std::to_string(index);
	if (index >= size())
	{
		std::ostringstream oss;
		oss << "Invalid index for " << OSTR(getPathToSelf()) << ". Index " << sindex << " exceeds array size.";
		throw ncp::exception(oss.str());
	}
	return JsonMember((*value)[index], this, sindex);
}

int JsonMember::getInt() const
{
	rj::Type type = value->GetType();
	if (type == rj::kNumberType)
	{
		return value->GetInt();
	}
	else if (type == rj::kStringType)
	{
		std::string_view str = value->GetString();
		if (str.starts_with("0x"))
		{
			try {
				return std::stoi(&str[2], nullptr, 16);
			} catch (std::invalid_argument& e) {}
		}
	}
	
	std::ostringstream oss;
	oss << "Invalid type for " << OSTR(getPathToSelf()) << ", expected " ANSI_bCYAN << "integer" ANSI_RESET " or " ANSI_bCYAN "hex string" << ANSI_RESET;
	throw ncp::exception(oss.str());
}

bool JsonMember::getBool() const
{
	if (!value->IsBool())
	{
		std::ostringstream oss;
		oss << "Invalid type for " << OSTR(getPathToSelf()) << ", expected " ANSI_bCYAN << "bool" << ANSI_RESET;
		throw ncp::exception(oss.str());
	}
	return value->GetBool();
}

const char* JsonMember::getString() const
{
	if (!value->IsString())
	{
		std::ostringstream oss;
		oss << "Invalid type for " << OSTR(getPathToSelf()) << ", expected " ANSI_bCYAN << "string" << ANSI_RESET;
		throw ncp::exception(oss.str());
	}
	return value->GetString();
}

const std::vector<JsonMember> JsonMember::getObjectArray() const
{
	assertArray();

	std::vector<JsonMember> out;

	const rapidjson::Value& entries = *value;
	const rapidjson::SizeType entryCount = entries.Size();

	for (rapidjson::SizeType i = 0; i < entryCount; i++)
	{
		const rapidjson::Value& entry = entries[i];
		if (!entry.IsObject())
		{
			std::ostringstream oss;
			oss << "Invalid value type in array " << OSTR(getPathToSelf()) << " at index " << i << ", expected " ANSI_bCYAN << "object" << ANSI_RESET;
			throw ncp::exception(oss.str());
		}

		out.emplace_back(entry, this, std::to_string(i));
	}

	return out;
}

const std::vector<JsonMember> JsonMember::getMembers() const
{
	assertObject();

	std::vector<JsonMember> nodes;

	const auto& object = value->GetObject();
	for (const auto& member : object)
		nodes.emplace_back(member.value, this, member.name.GetString());

	return nodes;
}

const std::string& JsonMember::getName() const
{
	return name;
}

size_t JsonMember::size() const
{
	assertArray();
	return value->Size();
}

bool JsonMember::hasMember(const char* member) const
{
	return value->HasMember(member);
}

bool JsonMember::isArray() const
{
	return value->IsArray();
}

bool JsonMember::isObject() const
{
	return value->IsObject();
}

void JsonMember::assertMember(const char* member) const
{
	if (!hasMember(member))
	{
		std::string path = getPathToSelf();
		if (path != "")
			path = path + "/" + member;
		else
			path = member;
		std::ostringstream oss;
		oss << OSTR(path) << " was not found.";
		throw ncp::exception(oss.str());
	}
}

void JsonMember::assertArray() const
{
	if (!isArray())
	{
		std::ostringstream oss;
		oss << OSTR(getPathToSelf()) << " is not an array.";
		throw ncp::exception(oss.str());
	}
}

void JsonMember::assertObject() const
{
	if (!isObject())
	{
		std::ostringstream oss;
		oss << OSTR(getPathToSelf()) << " is not an object.";
		throw ncp::exception(oss.str());
	}
}

std::string JsonMember::getPathToSelf() const
{
	std::string path;
	std::vector<std::string> names;

	const JsonMember* node = this;
	do {
		if (node->name != "")
			names.push_back(node->name);
		node = node->parent;
	} while (node != nullptr);

	size_t i = names.size();
	while (i-- != 0)
	{
		path += names[i];
		if (i != 0) path += "/";
	}
	
	return path;
}

// JsonReader

JsonReader::JsonReader(const fs::path& path)
{
	if (!fs::exists(path))
		throw ncp::file_error(path, ncp::file_error::find);

	std::ifstream file(path, std::ios::binary);
	if (!file.is_open())
		throw ncp::file_error(path, ncp::file_error::read);

	rj::IStreamWrapper isw(file);
	doc.ParseStream(isw);
	if (doc.HasParseError())
	{
		std::ostringstream oss;
		oss << rj::GetParseError_En(doc.GetParseError()) << " Offset: " << doc.GetErrorOffset();
		throw ncp::exception(oss.str());
	}

	file.close();

	root = JsonMember(doc, nullptr, "");
}

const JsonMember JsonReader::operator[](const char* member) const
{
	root.assertMember(member);
	return root[member];
}

const std::vector<JsonMember> JsonReader::getMembers() const
{
	std::vector<JsonMember> nodes;

	const auto& object = doc.GetObject();
	for (const auto& member : object)
		nodes.emplace_back(member.value, &root, member.name.GetString());

	return nodes;
}

bool JsonReader::hasMember(const char* member) const
{
	return root.hasMember(member);
}
