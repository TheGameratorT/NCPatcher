#include "ConfigManager.hpp"

#include <rapidjson/istreamwrapper.h>

#include <fstream>
#include <sstream>
#include <iostream>

#include "NCException.hpp"

namespace fs = std::filesystem;

static const char* OpenErr = "Could not open the configuration file.";
static const char* ParseErr = "Could not parse the build configuration.";

ConfigManager::ConfigManager(const std::filesystem::path& workDir)
{
	std::filesystem::current_path(workDir);

	std::filesystem::path configPath = "build_config.json";

	std::ifstream ifs(configPath);
	if (!ifs.is_open())
		throw NC::file_error(OpenErr, configPath, NC::file_error::read);
	
	rapidjson::IStreamWrapper isw(ifs);

	rapidjson::Document d;
	d.ParseStream(isw);

	getDirs(d, "SOURCES", sourceDirs);
	getDirs(d, "INCLUDES", includeDirs);

	getFlags(d, "CFLAGS", cFlags);
	getFlags(d, "CXXFLAGS", cxxFlags);
	getFlags(d, "ASFLAGS", asFlags);
	getFlags(d, "LDFLAGS", ldFlags);

	getString(d, "BUILD", build);
	getString(d, "TARGET", target);
	getString(d, "PREFIX", prefix);
	getString(d, "LINKER", linker);
	getString(d, "SYMBOLS", symbols);
	get(d, "MAIN_ARENA_LO_ADDR", arenaLoAddr);
}

void ConfigManager::assertMember(const rapidjson::Document& doc, const std::string& memberName)
{
	if (!doc.HasMember(memberName.c_str()))
	{
		std::ostringstream oss;
		oss << OSTR(memberName) << " was not found.";
		throw NC::exception(ParseErr, oss.str());
	}
}

void ConfigManager::assertArray(const rapidjson::Document& doc, const std::string& memberName)
{
	assertMember(doc, memberName);

	const auto& member = doc[memberName.c_str()];
	if (!member.IsArray())
	{
		std::ostringstream oss;
		oss << OSTR(memberName) << " is not an array.";
		throw NC::exception(ParseErr, oss.str());
	}
}

void ConfigManager::getDirsRecursive(const fs::path& path, std::vector<fs::path>& dirsOut)
{
	for (auto& entry : fs::directory_iterator(path))
	{
		if (entry.is_directory())
		{
			fs::path newPath = entry.path();
			dirsOut.push_back(newPath);
			getDirsRecursive(newPath, dirsOut);
		}
	}
}

void ConfigManager::getDirs(const rapidjson::Document& doc, const std::string& dirGroup, std::vector<fs::path>& dirsOut)
{
	assertArray(doc, dirGroup);

	const auto& entries = doc[dirGroup.c_str()];
	const int entryCount = entries.Size();

	if (entryCount != 0)
	{
		for (int i = 0; i < entryCount; i++)
		{
			const auto& entry = entries[i];
			if (!entry.IsArray() || entry.Size() != 2 || !entry[0].IsString() || !entry[1].IsBool())
			{
				std::ostringstream oss;
				oss << "Invalid directory entry for " << OSTR(dirGroup) << " at index " << i << ".";
				throw NC::exception(ParseErr, oss.str());
			}

			fs::path dirName(entry[0].GetString());
			bool recursive = entry[1].GetBool();

			if (!fs::exists(dirName))
			{
				std::cout << OWARN << "Ignored missing directory for " << OSTR(dirGroup) << ": " << OSTR(dirName.string()) << std::endl;
				continue;
			}

			dirsOut.push_back(dirName);
			if (recursive)
				getDirsRecursive(dirName, dirsOut);
		}
	}
}

void ConfigManager::getFlags(const rapidjson::Document& doc, const std::string& flagGroup, std::string& flagsOut)
{
	assertArray(doc, flagGroup);

	const auto& entries = doc[flagGroup.c_str()];
	const int entryCount = entries.Size();

	if (entryCount != 0)
	{
		for (int i = 0; i < entryCount; i++)
		{
			const auto& entry = entries[i];
			if (!entry.IsString())
			{
				std::ostringstream oss;
				oss << "Invalid flag for " << OSTR(flagGroup) << " at index " << i << ".";
				throw NC::exception(ParseErr, oss.str());
			}

			flagsOut += entry.GetString();
			flagsOut += ' ';
		}
	}
}

template<typename T>
void ConfigManager::get(const rapidjson::Document& doc, const std::string& name, T& out)
{
	assertMember(doc, name);

	const auto& entry = doc[name.c_str()];

	if (!entry.Is<T>())
	{
		std::ostringstream oss;
		oss << "Invalid type for " << OSTR(name);
		throw NC::exception(ParseErr, oss.str());
	}

	out = entry.Get<T>();
}

void ConfigManager::getString(const rapidjson::Document& doc, const std::string& name, std::string& out)
{
	assertMember(doc, name);

	const auto& entry = doc[name.c_str()];

	if (!entry.IsString())
	{
		std::ostringstream oss;
		oss << "Invalid type for " << OSTR(name);
		throw NC::exception(ParseErr, oss.str());
	}

	out = entry.GetString();
}
