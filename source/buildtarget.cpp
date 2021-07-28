#include "buildtarget.hpp"

#include <tuple>
#include <unordered_map>

#include "json.hpp"
#include "global.hpp"
#include "oansistream.hpp"

namespace fs = std::filesystem;
namespace rj = rapidjson;

typedef std::vector<std::tuple<std::string, std::string>> varmap_t;

static const char* region_mode_strs[] = { "append", "replace", "create" };

/*static void print_map(const varmap_t& m)
{
	for (const auto& n : m)
		ansi::cout << std::get<0>(n) << " = " << std::get<1>(n) << "\n";
}*/

static void expand_templates(const varmap_t& varmap, std::string& val)
{
	size_t varmaps = varmap.size();
	for (size_t i = 0; i < varmaps; i++)
	{
		const std::string& name = std::get<0>(varmap[i]);
		const std::string& value = std::get<1>(varmap[i]);

		size_t pos = 0;
		std::string id = "${" + name + "}";
		while ((pos = val.find(id, pos)) != std::string::npos)
		{
		    val.replace(pos, id.size(), value);
		    pos += value.size();
		}
	}
}

static std::string get_string(const varmap_t& varmap, const JsonMember& member)
{
	std::string out = member.getString();
	expand_templates(varmap, out);
	return out;
}

static void add_path_recursively(const fs::path& path, std::vector<fs::path>& out)
{
	for (const auto& subdir : fs::directory_iterator(path))
	{
		if (subdir.is_directory())
		{
			fs::path newPath = subdir.path();
			newPath.make_preferred();
			out.push_back(newPath);
			add_path_recursively(newPath, out);
		}
	}
}

static void get_directory_array(const varmap_t& varmap, const JsonMember& member, std::vector<fs::path>& out)
{
	size_t size = member.size();
	for (size_t i = 0; i < size; i++)
	{
		JsonMember info = member[i];
		fs::path path = get_string(varmap, info[0]);
		path.make_preferred();
		if (!fs::exists(path))
		{
			ansi::cout << OWARN << "Ignored non-existent directory: " << OSTR(path.string()) << std::endl;
			continue;
		}

		bool recursive = info[1].getBool();
		out.push_back(path);
		if (recursive)
			add_path_recursively(path, out);
	}
}

static void read_region_mode(BuildTarget::Region& region, const JsonMember& member)
{
	if (member.hasMember("mode"))
	{
		const char* modeStr = member["mode"].getString();
		size_t index = util::index_of(modeStr, region_mode_strs, 3);
		if (index != -1)
		{
			region.mode = static_cast<BuildTarget::Mode>(index);
			return;
		}
		
		std::ostringstream oss;
		oss << OERROR << "Invalid mode " << modeStr << ".";
		throw ncp::exception(oss.str());
	}
	region.mode = BuildTarget::Mode::append;
}

BuildTarget::BuildTarget(const fs::path& path)
{
	fs::current_path(path);

	JsonReader json("target.json");

	varmap_t varmap;
	varmap.emplace_back("asm", ncp::asm_path.string());
	varmap.emplace_back("rom", ncp::rom_path.string());

	const std::vector<JsonMember> members = json.getMembers();
	for (const JsonMember& member : members)
	{
		std::string name = std::string(member.getName());
		if (name.size() > 1 && name[0] == '$')
			varmap.emplace_back(name.substr(1), get_string(varmap, member));
	}

	arenaLo = json["arenaLo"].getInt();
	symbols = get_string(varmap, json["symbols"]);
	build = get_string(varmap, json["build"]);

	symbols.make_preferred();
	build.make_preferred();

	get_directory_array(varmap, json["includes"], includes);

	const std::vector<JsonMember> regionObjs = json["regions"].getObjectArray();
	for (const JsonMember& regionObj : regionObjs)
	{
		Region region;
		get_directory_array(varmap, regionObj["sources"], region.sources);
		region.destination = get_string(varmap, regionObj["destination"]);
		region.compress = regionObj["compress"].getBool();
		region.cFlags = get_string(varmap, regionObj["c_flags"]);
		region.cppFlags = get_string(varmap, regionObj["cpp_flags"]);
		region.asmFlags = get_string(varmap, regionObj["asm_flags"]);
		region.ldFlags = get_string(varmap, regionObj["ld_flags"]);
		read_region_mode(region, regionObj);
		regions.push_back(region);
	}
}
