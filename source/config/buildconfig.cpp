#include "buildconfig.hpp"

#include <unordered_map>
#include <string>
#include <vector>
#include <filesystem>
#include <sstream>

#include "../app/application.hpp"
#include "json.hpp"
#include "../system/log.hpp"
#include "../system/except.hpp"
#include "../utils/util.hpp"

namespace fs = std::filesystem;

using varmap_t = std::unordered_map<std::string, std::string>;

namespace BuildConfig {

static const char* s_loadErr = "Could not load the build configuration.";
static const char* s_jsonFileName = "ncpatcher.json";

struct TargetConfig
{
	bool doBuild = false;
	fs::path target;
	fs::path build;
	fs::path workdir;
};

static varmap_t varmap;
static fs::path backupDir;
static fs::path filesystemDir;
static std::string toolchain;
static TargetConfig arm7Config;
static TargetConfig arm9Config;
static std::vector<std::string> preBuildCmds;
static std::vector<std::string> postBuildCmds;
static int threadCount;
static std::time_t lastWriteTime;

static void expandTemplates(std::string& val)
{
	auto throwInvalidExpansion = [&val](){
		std::ostringstream oss;
		oss << "Invalid variable template expansion in string " << OSTR(val) << " in " << OSTR(s_jsonFileName);
		throw ncp::exception(oss.str());
	};

	size_t pos = 0;
	while ((pos = val.find('$', pos)) != std::string::npos)
	{
		if (pos + 4 > val.size())
			break;
		if (val[pos + 1] != '{')
			throwInvalidExpansion();

		size_t endpos = val.find('}', pos + 1);
		if (endpos == std::string::npos)
			break;

		std::string varname = val.substr(pos + 2, endpos - (pos + 2));
		std::string varvalue;
		if (varname.starts_with("env:"))
		{
			if (varname.size() == 4)
				throwInvalidExpansion();
			std::string envvarname = varname.substr(4);
			const char* envvarvalue = std::getenv(envvarname.c_str());
			if (envvarvalue == nullptr)
			{
				std::ostringstream oss;
				oss << "Could not find environment variable " << OSTR(envvarname) << " referenced in " << OSTR(s_jsonFileName);
				throw ncp::exception(oss.str());
			}
			varvalue = envvarvalue;
		}
		else
		{
			varvalue = getVariable(varname);
		}
		val.replace(pos, endpos - pos + 1, varvalue);
		pos += varvalue.size();
	}
}

static std::string getString(const JsonMember& member)
{
	std::string out = member.getString();
	expandTemplates(out);
	return out;
}

static void readTargetFromJson(JsonReader& json, bool arm9, TargetConfig& out)
{
	const char* nodeName = arm9 ? "arm9" : "arm7";

	if (json.hasMember(nodeName))
	{
		JsonMember jsonNode = json[nodeName];
		if (!jsonNode.isNull() && jsonNode.isObject() && jsonNode.memberCount() > 0)
		{
			out.target = getString(jsonNode["target"]);
			out.build = getString(jsonNode["build"]);
			// Optional workdir - if present, resolve relative to application work path when used
			if (jsonNode.hasMember("workdir") && !jsonNode["workdir"].isNull())
				out.workdir = getString(jsonNode["workdir"]);
			out.doBuild = true;
		}
	}
}

static void readBuildCommands(const JsonMember& member, std::vector<std::string>& cmdsOut)
{
	size_t size = member.size();
	for (size_t i = 0; i < size; i++)
		cmdsOut.emplace_back(getString(member[i]));
}

void load()
{
	ncp::Application::setErrorContext(s_loadErr);

	Log::info("Loading build configuration...");

	fs::path jsonPath = ncp::Application::getWorkPath() / s_jsonFileName;

	JsonReader json(jsonPath);

	varmap.emplace("root", ncp::Application::getWorkPath().string());

	std::vector<JsonMember> members = json.getMembers();
	for (const JsonMember& member : members)
	{
		std::string name = std::string(member.getName());
		if (name.size() > 1 && name[0] == '$')
			varmap.emplace(name.substr(1), getString(member));
	}

	backupDir = getString(json["backup"]);
	filesystemDir = getString(json["filesystem"]);
	toolchain = getString(json["toolchain"]);

	readTargetFromJson(json, false, arm7Config);
	readTargetFromJson(json, true, arm9Config);

	if (!arm7Config.doBuild && !arm9Config.doBuild)
		throw ncp::exception("No targets to build were specified.");

	readBuildCommands(json["pre-build"], preBuildCmds);
	readBuildCommands(json["post-build"], postBuildCmds);

	threadCount = json["thread-count"].getInt();

	lastWriteTime = Util::toTimeT(fs::last_write_time(jsonPath));

	ncp::Application::setErrorContext(nullptr);
}

const std::string& getVariable(const std::string& value)
{
	try {
		return varmap.at(value);
	} catch (std::exception& ex) {
		std::ostringstream oss;
		oss << "Could not find variable " << OSTR(value) << " in " << OSTR(s_jsonFileName);
		throw ncp::exception(oss.str());
	}
}

const fs::path& getBackupDir() { return backupDir; }
const fs::path& getFilesystemDir() { return filesystemDir; }
const std::string& getToolchain() { return toolchain; }

bool getBuildArm7() { return arm7Config.doBuild; }
const fs::path& getArm7Target() { return arm7Config.target; }
const fs::path& getArm7BuildDir() { return arm7Config.build; }
const fs::path& getArm7WorkDir() { return arm7Config.workdir; }

bool getBuildArm9() { return arm9Config.doBuild; }
const fs::path& getArm9Target() { return arm9Config.target; }
const fs::path& getArm9BuildDir() { return arm9Config.build; }
const fs::path& getArm9WorkDir() { return arm9Config.workdir; }

const std::vector<std::string>& getPreBuildCmds() { return preBuildCmds; }
const std::vector<std::string>& getPostBuildCmds() { return postBuildCmds; }

int getThreadCount() { return threadCount; }
std::time_t getLastWriteTime() { return lastWriteTime; }

}
