#include "buildtarget.hpp"

#include <array>
#include <sstream>
#include <cstdlib>

#include "../app/application.hpp"
#include "../system/log.hpp"
#include "../system/except.hpp"
#include "../utils/util.hpp"
#include "buildconfig.hpp"

namespace fs = std::filesystem;
namespace rj = rapidjson;

using varmap_t = std::unordered_map<std::string, std::string>;

static const char* s_regionModeStrs[] = { "append", "replace", "create" };

BuildTarget::BuildTarget() = default;

void BuildTarget::load(const fs::path& targetFilePath, const fs::path& targetWorkDir, bool isArm9)
{
	m_isArm9 = isArm9;

	fs::path curPath = fs::current_path();

	fs::current_path(ncp::Application::getWorkPath());
	fs::current_path(targetWorkDir);

	JsonReader json(targetFilePath);

	varmap.emplace("root", ncp::Application::getWorkPath().string());

	const std::vector<JsonMember> members = json.getMembers();
	for (const JsonMember& member : members)
	{
		std::string name = std::string(member.getName());
		if (name.size() > 1 && name[0] == '$')
			varmap.emplace(name.substr(1), getString(member));
	}

	arenaLo = json.hasMember("arenaLo") ? json["arenaLo"].getInt() : 0;
	
	if (json.hasMember("symbols"))
	{
		symbols = getString(json["symbols"]);
		symbols.make_preferred();
	}

	getDirectoryArray(json["includes"], includes, true);

	cFlags = getString(json["c_flags"]);
	cppFlags = getString(json["cpp_flags"]);
	asmFlags = getString(json["asm_flags"]);
	ldFlags = getString(json["ld_flags"]);

	std::vector<JsonMember> regionObjs = json["regions"].getObjectArray();
	for (JsonMember& regionObj : regionObjs)
	{
		Region region;
		getDirectoryArray(regionObj["sources"], region.sources, false);
		readDestination(region, regionObj["dest"]);
		region.compress = regionObj["compress"].getBool();
		region.cFlags = regionObj.hasMember("c_flags") ? getString(regionObj["c_flags"]) : cFlags;
		region.cppFlags = regionObj.hasMember("cpp_flags") ? getString(regionObj["cpp_flags"]) : cppFlags;
		region.asmFlags = regionObj.hasMember("asm_flags") ? getString(regionObj["asm_flags"]) : asmFlags;
		//region.ldFlags = regionObj.hasMember("ld_flags") ? getString(regionObj["ld_flags"]) : ldFlags;
		readRegionMode(region, regionObj);
		if (region.mode == Mode::Replace)
			region.address = regionObj.hasMember("address") ? regionObj["address"].getInt() : 0xFFFFFFFF;
		else
			region.address = (region.mode == Mode::Create) ? regionObj["address"].getInt() : 0;
		region.maxsize = regionObj.hasMember("maxsize") ? regionObj["maxsize"].getInt() : 0x100000;
		readOverwrites(region, regionObj);
		regions.push_back(region);
	}

	m_lastWriteTime = Util::toTimeT(fs::last_write_time(targetFilePath));

	fs::current_path(curPath);
}

bool BuildTarget::hasOverwrites() const
{
	for (const auto& region : regions)
	{
		if (region.overwrites.size() != 0)
			return true;
	}
	return false;
}

const BuildTarget::Region* BuildTarget::getRegionByDestination(int destination) const
{
	for (const auto& region : regions)
	{
		if (region.destination == destination)
			return &region;
	}
	return nullptr;
}

BuildTarget::Region* BuildTarget::getRegionByDestination(int destination)
{
	for (auto& region : regions)
	{
		if (region.destination == destination)
			return &region;
	}
	return nullptr;
}

const BuildTarget::Region* BuildTarget::getMainRegion() const
{
	return getRegionByDestination(-1);
}

BuildTarget::Region* BuildTarget::getMainRegion()
{
	return getRegionByDestination(-1);
}

const std::string& BuildTarget::getVariable(const std::string& value)
{
	try {
		return varmap.at(value);
	} catch (std::exception& ex) {
		std::ostringstream oss;
		oss << "Could not find variable " << OSTR(value) << " in the " << OSTR(m_isArm9 ? "arm9" : "arm7") << " target.";
		throw ncp::exception(oss.str());
	}
}

void BuildTarget::expandTemplates(std::string& val)
{
	auto throwInvalidExpansion = [this, &val](){
		std::ostringstream oss;
		oss << "Invalid variable template expansion in string " << OSTR(val) << " in the " << OSTR(m_isArm9 ? "arm9" : "arm7") << " target.";
		throw ncp::exception(oss.str());
	};

	size_t pos = 0;
	while ((pos = val.find('$', pos)) != std::string::npos)
	{
		if (pos + 1 > val.size())
			break;
		int off = val[pos + 1] == '$';
		if (pos + off + 4 > val.size())
			break;
		if (val[pos + off + 1] != '{')
			throwInvalidExpansion();
		
		size_t endpos = val.find('}', pos + off + 1);
		if (endpos == std::string::npos)
			break;
		std::string varname = val.substr(pos + off + 2, endpos - (pos + off + 2));
		bool env = varname.starts_with("env:");
		if (off && env)
			throwInvalidExpansion();

		std::string varvalue;
		if (env)
		{
			if (varname.size() == 4)
				throwInvalidExpansion();
			std::string envvarname = varname.substr(4);
			const char* envvarvalue = std::getenv(envvarname.c_str());
			if (envvarvalue == nullptr)
			{
				std::ostringstream oss;
				oss << "Could not find environment variable " << OSTR(envvarname) << " referenced in the " << OSTR(m_isArm9 ? "arm9" : "arm7") << " target.";
				throw ncp::exception(oss.str());
			}
			varvalue = envvarvalue;
		}
		else
		{
			varvalue = off ? BuildConfig::getVariable(varname) : getVariable(varname);
		}
		val.replace(pos, endpos - pos + 1, varvalue);
		pos += varvalue.size();
	}
}

std::string BuildTarget::getString(const JsonMember& member)
{
	std::string out = member.getString();
	expandTemplates(out);
	return out;
}

void BuildTarget::getDirectoryArray(const JsonMember& member, std::vector<fs::path>& out, bool directoriesOnly)
{
    size_t size = member.size();
    for (size_t i = 0; i < size; i++)
    {
        std::string pattern = getString(member[i]);
        fs::path patternPath = pattern;
        patternPath.make_preferred();

        // Only support * and ** for globbing
        auto hasGlob = [](const std::string& s) {
            return s.find('*') != std::string::npos;
        };

        if (!hasGlob(pattern))
        {
            // No glob: treat as literal path
            if (!fs::exists(patternPath))
            {
                Log::out << OWARN << "Ignored non-existent path: " << OSTR(patternPath.string()) << std::endl;
                continue;
            }
            if (directoriesOnly)
            {
                if (fs::is_directory(patternPath))
                    out.push_back(patternPath);
                else
                    Log::out << OWARN << "Ignored non-directory path for includes: " << OSTR(patternPath.string()) << std::endl;
            }
            else
            {
                if (fs::is_regular_file(patternPath))
                    out.push_back(patternPath);
                else if (fs::is_directory(patternPath))
                {
                    for (const auto& entry : fs::directory_iterator(patternPath))
                    {
                        if (entry.is_regular_file())
                            out.push_back(entry.path());
                    }
                }
            }
            continue;
        }

        // Find base directory (up to first *)
        size_t firstStar = pattern.find('*');
        size_t baseSlash = pattern.rfind('/', firstStar);
        fs::path baseDir = (baseSlash == std::string::npos) ? fs::current_path() : fs::path(pattern.substr(0, baseSlash));
        std::string matchPattern = (baseSlash == std::string::npos) ? pattern : pattern.substr(baseSlash + 1);

        if (!fs::exists(baseDir))
        {
            Log::out << OWARN << "Ignored non-existent path: " << OSTR(baseDir.string()) << std::endl;
            continue;
        }

        bool recursive = matchPattern.find("**") != std::string::npos;

        auto match = [](const std::string& pat, const std::string& name) {
            // Only supports '*' wildcard
            if (pat == "*") return true;
            size_t star = pat.find('*');
            if (star == std::string::npos) return pat == name;
			std::string before = pat.substr(0, star);
			std::string after = pat.substr(star + 1);
			if (!before.empty() && !name.starts_with(before)) return false;
			if (!after.empty() && !name.ends_with(after)) return false;
			return name.size() >= before.size() + after.size();
        };

        if (recursive)
        {
            for (const auto& entry : fs::recursive_directory_iterator(baseDir))
            {
                if (directoriesOnly && !entry.is_directory()) continue;
                if (!directoriesOnly && !entry.is_regular_file()) continue;
                std::string fname = entry.path().filename().string();
                if (match(matchPattern, fname))
                    out.push_back(entry.path());
            }
        }
        else
        {
            for (const auto& entry : fs::directory_iterator(baseDir))
            {
                if (directoriesOnly && !entry.is_directory()) continue;
                if (!directoriesOnly && !entry.is_regular_file()) continue;
                std::string fname = entry.path().filename().string();
                if (match(matchPattern, fname))
                    out.push_back(entry.path());
            }
        }
    }
}

void BuildTarget::readDestination(BuildTarget::Region& region, const JsonMember& member)
{
	const char* destStr = member.getString();
	std::string_view destStrV(destStr);
	if (destStrV.starts_with("ov"))
	{
		try {
			region.destination = std::stoi(&destStr[2], nullptr, 10);
		} catch (const std::exception& e) {
			throw ncp::exception("Invalid overlay ID for destination.");
		}
		return;
	}
	if (destStrV == "main")
	{
		region.destination = -1;
		return;
	}
	throw ncp::exception(R"(Invalid destination, use either "main" or "ovXX".)");
}

void BuildTarget::readRegionMode(BuildTarget::Region& region, const JsonMember& member)
{
	if (member.hasMember("mode"))
	{
		const char* modeStr = member["mode"].getString();
		size_t index = Util::indexOf(modeStr, s_regionModeStrs, 3);
		if (index != -1)
		{
			region.mode = static_cast<BuildTarget::Mode>(index);
			return;
		}

		std::ostringstream oss;
		oss << OERROR << "Invalid mode " << modeStr << ".";
		throw ncp::exception(oss.str());
	}
	region.mode = BuildTarget::Mode::Append;
}

void BuildTarget::readOverwrites(BuildTarget::Region& region, const JsonMember& member)
{
	if (member.hasMember("overwrites"))
	{
		JsonMember overwritesArray = member["overwrites"];
		size_t overwriteCount = overwritesArray.size();
		for (size_t i = 0; i < overwriteCount; i++)
		{
			JsonMember overwritePair = overwritesArray[i];
			
			Overwrites overwrite;
			overwrite.startAddress = overwritePair[size_t(0)].getInt();
			overwrite.endAddress = overwritePair[size_t(1)].getInt();

			if (overwrite.startAddress == overwrite.endAddress)
			{
				std::ostringstream oss;
				oss << OERROR << "Overwrite startAddress " << OSTR(overwrite.startAddress) << " must not be the same as the endAddress " << OSTR(overwrite.endAddress) << ".";
				throw ncp::exception(oss.str());
			}
			else if (overwrite.startAddress > overwrite.endAddress)
			{
				std::ostringstream oss;
				oss << OERROR << "Overwrite startAddress " << OSTR(overwrite.startAddress) << " must not be higher than the endAddress " << OSTR(overwrite.endAddress) << ".";
				throw ncp::exception(oss.str());
			}

			region.overwrites.push_back(overwrite);
		}
	}
}
