#include "buildcfg.hpp"

#include <fstream>

#include "common.hpp"
#include "except.hpp"
#include "json.hpp"
#include "oansistream.hpp"
#include "global.hpp"

namespace fs = std::filesystem;

static const char* LoadErr = "Could not load the build configuration.";

void BuildCfg::load(const fs::path& path)
{
	ncp::setErrorMsg(LoadErr);

	ansi::cout << OINFO << "Loading build configuration..." << std::endl;

	JsonReader json(path);

	backup = json["backup"].getString();
	arm7 = json["arm7"].getString();
	arm9 = json["arm9"].getString();

	ansi::cout << OINFO << "Backup directory: " << OSTR(backup.string()) << std::endl;

	if (arm7 != "")
	{
		if (!fs::exists(arm7))
			throw ncp::dir_error(arm7, ncp::dir_error::find);
		ansi::cout << OINFO << "ARM7 workspace: " << OSTR(arm7.string()) << std::endl;
	}

	if (arm9 != "")
	{
		if (!fs::exists(arm9))
			throw ncp::dir_error(arm9, ncp::dir_error::find);
		ansi::cout << OINFO << "ARM9 workspace: " << OSTR(arm9.string()) << std::endl;
	}
}
