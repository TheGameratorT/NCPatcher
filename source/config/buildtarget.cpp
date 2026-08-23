#include "buildtarget.hpp"

#include <array>
#include <sstream>
#include <cstdlib>

#include "../app/application.hpp"
#include "../system/log.hpp"
#include "../system/except.hpp"
#include "../utils/util.hpp"
#include "../utils/glob.hpp"
#include "buildconfig.hpp"

namespace fs = std::filesystem;
namespace rj = rapidjson;

using varmap_t = std::unordered_map<std::string, std::string>;

static const char* s_regionModeStrs[] = { "append", "replace", "create" };

BuildTarget::BuildTarget() = default;

void BuildTarget::load(const fs::path& targetFilePath, const ncp::PathContext& paths, bool isArm9)
{
	m_isArm9 = isArm9;
	m_paths = &paths;

	JsonReader json(targetFilePath);

	varmap.emplace("root", paths.workDir.string());

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

	m_paths = nullptr;
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

void BuildTarget::readLegacyPathPair(const JsonMember& entry, bool directoriesOnly,
                                     std::vector<std::string>& out)
{
	// Releases before the glob syntax took [path, recursive] pairs. Six shipped
	// projects still use that form, so translate rather than reject it.
	if (entry.size() != 2)
	{
		std::ostringstream oss;
		oss << "Invalid entry " << OSTR(entry.getPathToSelf())
		    << ", expected a string pattern or a " ANSI_bCYAN "[path, recursive]" ANSI_RESET " pair.";
		throw ncp::exception(oss.str());
	}

	std::string path = getString(entry[size_t(0)]);
	const bool recursive = entry[size_t(1)].getBool();

	while (path.size() > 1 && (path.back() == '/' || path.back() == '\\'))
		path.pop_back();

	if (directoriesOnly)
	{
		// The directory itself, plus every subdirectory when recursive.
		out.push_back(path);
		if (recursive)
			out.push_back(path + "/**");
	}
	else
	{
		// Files directly inside the directory, or at any depth when recursive.
		out.push_back(recursive ? path + "/**" : path);
	}
}

void BuildTarget::getDirectoryArray(const JsonMember& member, std::vector<fs::path>& out, bool directoriesOnly)
{
	Glob::Options options;
	options.directoriesOnly = directoriesOnly;

	std::vector<fs::path> matched;
	std::vector<std::string> exclusions;

	const size_t size = member.size();
	for (size_t i = 0; i < size; i++)
	{
		const JsonMember entry = member[i];

		std::vector<std::string> patterns;
		if (entry.isArray())
			readLegacyPathPair(entry, directoriesOnly, patterns);
		else
			patterns.push_back(getString(entry));

		for (const std::string& pattern : patterns)
		{
			if (pattern.starts_with('!'))
			{
				exclusions.emplace_back(pattern.substr(1));
				continue;
			}

			// Searched under the target work directory, but kept relative to it:
			// object paths are derived from these.
			std::vector<fs::path> found = Glob::expand(pattern, m_paths->targetWorkDir, options);
			if (found.empty())
			{
				Log::out << OWARN << (Glob::hasWildcard(pattern)
					? "Pattern matched nothing: "
					: "Ignored non-existent path: ") << OSTR(pattern) << std::endl;
				continue;
			}
			matched.insert(matched.end(), found.begin(), found.end());
		}
	}

	for (const fs::path& path : matched)
	{
		const std::string generic = path.generic_string();
		bool excluded = false;
		for (const std::string& exclusion : exclusions)
		{
			if (Glob::match(exclusion, generic))
			{
				excluded = true;
				break;
			}
		}
		if (!excluded)
			out.push_back(path);
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
