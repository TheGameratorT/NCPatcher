#pragma once

#include "Common.hpp"

#include <rapidjson/document.h>

#include <string>
#include <vector>
#include <filesystem>

class ConfigManager
{
public:
	ConfigManager(const std::filesystem::path& path);

	std::vector<std::filesystem::path> sourceDirs;
	std::vector<std::filesystem::path> includeDirs;

	std::string cFlags;
	std::string cxxFlags;
	std::string asFlags;
	std::string ldFlags;

	std::string build;
	std::string target;
	std::string prefix;
	std::string linker;
	std::string symbols;

	u32 arenaLoAddr;

private:
	void assertMember(const rapidjson::Document& doc, const std::string& memberName);
	void assertArray(const rapidjson::Document& doc, const std::string& memberName);
	void getDirsRecursive(const std::filesystem::path& path, std::vector<std::filesystem::path>& dirsOut);
	void getDirs(const rapidjson::Document& doc, const std::string& dirGroup, std::vector<std::filesystem::path>& dirsOut);
	void getFlags(const rapidjson::Document& doc, const std::string& flagGroup, std::string& flagsOut);
	template<typename T> void get(const rapidjson::Document& doc, const std::string& name, T& out);
	void getString(const rapidjson::Document& doc, const std::string& name, std::string& out);
};
