#include "config_loader.hpp"

#include <sstream>

#include "node.hpp"
#include "../system/log.hpp"

namespace fs = std::filesystem;

namespace ncp::config {

fs::path findProjectFile(const fs::path& projectRoot)
{
	// v2 first, so a migrated project stops reading the old file the moment the
	// new one exists rather than when someone remembers to delete it.
	static const char* names[] = { "ncpatcher.yaml", "ncpatcher.yml", "ncpatcher.json" };

	for (const char* name : names)
	{
		const fs::path candidate = projectRoot / name;
		if (fs::exists(candidate))
			return candidate;
	}

	return {};
}

ProjectConfig load(const fs::path& projectFile, const fs::path& projectRoot)
{
	// Which schema a file is written in is decided by what is in it, not by its
	// extension: a project that renamed ncpatcher.json to ncpatcher.yaml before
	// converting the contents must still be read correctly, and told so.
	//
	// This parses the file a second time inside the chosen reader. Project
	// configs are a few kilobytes; keeping each reader able to open its own
	// input is worth more than the microseconds.
	int version = 1;
	{
		const cfg::Document doc(projectFile);
		const cfg::Node root = doc.root();
		if (!root.isMap())
			root.failType("a mapping");
		if (root.has("version"))
			version = root["version"].asInt();
	}

	if (version >= 2)
		return loadV2(projectFile, projectRoot);

	Log::out << OWARN << OSTR(projectFile.filename().string())
	         << " uses the version 1 schema." OREASONNL "Run "
	            ANSI_bCYAN "ncpatcher migrate" ANSI_RESET " to convert it; version 1 keeps working"
	            " for now." << std::endl;

	return loadV1(projectFile, projectRoot);
}

} // namespace ncp::config
