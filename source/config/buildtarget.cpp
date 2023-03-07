#include "buildtarget.hpp"

#include <array>
#include <sstream>

#include "../main.hpp"
#include "../log.hpp"
#include "../except.hpp"
#include "../util.hpp"
#include "buildconfig.hpp"

namespace fs = std::filesystem;
namespace rj = rapidjson;

using varmap_t = std::unordered_map<std::string, std::string>;

static const char* s_regionModeStrs[] = { "append", "replace", "create" };

BuildTarget::BuildTarget() = default;

void BuildTarget::load(const fs::path& targetFilePath, bool isArm9)
{
	m_isArm9 = isArm9;

	fs::path curPath = fs::current_path();

	fs::current_path(Main::getWorkPath());
	fs::current_path(targetFilePath.parent_path());

	fs::path targetFileName = targetFilePath.filename();

	JsonReader json(targetFileName);

	varmap.emplace("root", Main::getWorkPath().string());

	const std::vector<JsonMember> members = json.getMembers();
	for (const JsonMember& member : members)
	{
		std::string name = std::string(member.getName());
		if (name.size() > 1 && name[0] == '$')
			varmap.emplace(name.substr(1), getString(member));
	}

	arenaLo = json["arenaLo"].getInt();
	if (json.hasMember("symbols"))
	{
		symbols = getString(json["symbols"]);
		symbols.make_preferred();
	}

	getDirectoryArray(json["includes"], includes);

	cFlags = getString(json["c_flags"]);
	cppFlags = getString(json["cpp_flags"]);
	asmFlags = getString(json["asm_flags"]);
	ldFlags = getString(json["ld_flags"]);

	std::vector<JsonMember> regionObjs = json["regions"].getObjectArray();
	for (JsonMember& regionObj : regionObjs)
	{
		Region region;
		getDirectoryArray(regionObj["sources"], region.sources);
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
		region.length = regionObj.hasMember("length") ? regionObj["length"].getInt() : 0x100000;
		regions.push_back(region);
	}

	m_lastWriteTime = Util::toTimeT(fs::last_write_time(targetFileName));

	fs::current_path(curPath);
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
	size_t pos = 0;
	while ((pos = val.find('$', pos)) != std::string::npos)
	{
		if (pos + 1 > val.size())
			break;
		int off = val[pos + 1] == '$';
		if (pos + off + 4 > val.size())
			break;
		if (val[pos + off + 1] == '{')
		{
			size_t endpos = val.find('}', pos + off + 1);
			if (endpos == std::string::npos)
				break;
			std::string varname = val.substr(pos + off + 2, endpos - (pos + off + 2));
			const std::string& varvalue = off ? BuildConfig::getVariable(varname) : getVariable(varname);
			val.replace(pos, endpos - pos + 1, varvalue);
			pos += varvalue.size();
		}
	}
}

std::string BuildTarget::getString(const JsonMember& member)
{
	std::string out = member.getString();
	expandTemplates(out);
	return out;
}

void BuildTarget::addPathRecursively(const fs::path& path, std::vector<fs::path>& out)
{
	for (const auto& subdir : fs::directory_iterator(path))
	{
		if (subdir.is_directory())
		{
			fs::path newPath = subdir.path();
			newPath.make_preferred();
			out.push_back(newPath);
			addPathRecursively(newPath, out);
		}
	}
}

void BuildTarget::getDirectoryArray(const JsonMember& member, std::vector<fs::path>& out)
{
	size_t size = member.size();
	for (size_t i = 0; i < size; i++)
	{
		JsonMember info = member[i];
		fs::path path = getString(info[size_t(0)]);
		path.make_preferred();
		if (!fs::exists(path))
		{
			Log::out << OWARN << "Ignored non-existent directory: " << OSTR(path.string()) << std::endl;
			continue;
		}

		bool recursive = info[1].getBool();
		out.push_back(path);
		if (recursive)
			addPathRecursively(path, out);
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
