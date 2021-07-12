#include "buildtarget.hpp"

#include <tuple>
#include <unordered_map>

#include "json.hpp"
#include "global.hpp"
#include "oansistream.hpp"

namespace fs = std::filesystem;
namespace rj = rapidjson;

typedef std::vector<std::tuple<std::string, std::string>> varmap_t;

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
		if (!fs::exists(path))
		{
			ansi::cout << OWARN << "Ignored non-existent directory: " << OSTR(path.string()) << std::endl;
			return;
		}

		bool recursive = info[1].getBool();
		out.push_back(path);
		if (recursive)
			add_path_recursively(path, out);
	}
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

	get_directory_array(varmap, json["includes"], includes);
}
