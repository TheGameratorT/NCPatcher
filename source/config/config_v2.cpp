// The v2 reader: one ncpatcher.yaml.
//
// The two things this buys over v1 are real inheritance and defines as data.
// v1 had neither, which is why every project that exists carries the same
// hand-written string-concatenation boilerplate across two files: `$c_flags`
// declared in the project, re-declared in the target as "${arm_flags}
// $${c_flags} -DSDK_ARM9 ...", then assigned to `c_flags` -- three lines to
// express "the project's flags, plus these". Here that is what happens by
// default, and the mapping form is there for when it is not what you want.
//
// Unknown keys are errors rather than warnings. A misspelled `maxsize` in v1
// silently took the default and let an overlay overflow the space reserved for
// it; there is no reason to keep offering that.

#include "config_loader.hpp"

#include <initializer_list>
#include <sstream>

#include "expander.hpp"
#include "../system/log.hpp"
#include "../system/except.hpp"

namespace fs = std::filesystem;

namespace ncp::config {

namespace {

// Sections that belong to phases this build does not have yet. Naming them
// explicitly means a config written for a later version fails with what is
// missing rather than with "unknown key".
constexpr std::string_view RESERVED_KEYS[] = { "modules", "files", "hooks", "variants" };

bool isReserved(std::string_view key)
{
	for (std::string_view reserved : RESERVED_KEYS)
	{
		if (key == reserved)
			return true;
	}
	return false;
}

void checkKeys(const cfg::Node& node, std::string_view what,
               std::initializer_list<std::string_view> known)
{
	for (const auto& [key, value] : node.fields())
	{
		bool found = false;
		for (std::string_view candidate : known)
		{
			if (key == candidate)
			{
				found = true;
				break;
			}
		}
		if (found)
			continue;

		std::ostringstream oss;
		if (isReserved(key))
		{
			oss << OSTR(key) << " is not supported by this version of NCPatcher.";
		}
		else
		{
			oss << "Unknown key " << OSTR(key) << " in " << what << "." OREASONNL "Expected one of: ";
			bool first = true;
			for (std::string_view candidate : known)
			{
				oss << (first ? "" : ", ") << candidate;
				first = false;
			}
		}
		value.fail(oss.str());
	}
}

std::vector<std::string> readStrings(const cfg::Node& node, const Expander& expander)
{
	std::vector<std::string> out;
	if (!node.defined() || node.isNull())
		return out;

	if (node.isScalar())
	{
		out.push_back(expander.expand(node.asString(), node));
		return out;
	}

	for (const cfg::Node& item : node.items())
		out.push_back(expander.expand(item.asString(), item));

	return out;
}

// A list-valued setting: a bare sequence appends, the mapping form spells out
// what else it could mean.
ListOp readListOp(const cfg::Node& node, const Expander& expander)
{
	ListOp out;
	if (!node.defined() || node.isNull())
		return out;

	if (node.isMap())
	{
		checkKeys(node, "a list operation", { "set", "remove", "append" });
		if (node.has("set"))
		{
			out.hasSet = true;
			out.set = readStrings(node["set"], expander);
		}
		out.remove = readStrings(node["remove"], expander);
		out.append = readStrings(node["append"], expander);
		return out;
	}

	out.append = readStrings(node, expander);
	return out;
}

FlagOps readFlagOps(const cfg::Node& node, const Expander& expander)
{
	FlagOps out;
	if (!node.defined() || node.isNull())
		return out;

	if (!node.isMap())
		node.failType("a mapping of " ANSI_bCYAN "common/c/cpp/asm/ld" ANSI_RESET " lists");

	checkKeys(node, "flags", { "common", "c", "cpp", "asm", "ld" });
	out.common = readListOp(node["common"], expander);
	out.c = readListOp(node["c"], expander);
	out.cpp = readListOp(node["cpp"], expander);
	out.asm_ = readListOp(node["asm"], expander);
	out.ld = readListOp(node["ld"], expander);
	return out;
}

void readDestination(RegionConfig& region, const cfg::Node& node)
{
	const std::string dest = node.asString();
	region.dest = dest;

	if (dest == "main")
	{
		region.destination = -1;
		return;
	}

	if (dest.starts_with("ov"))
	{
		const std::string digits = dest.substr(2);
		if (!digits.empty() && digits.find_first_not_of("0123456789") == std::string::npos)
		{
			try {
				region.destination = std::stoi(digits);
				return;
			} catch (const std::exception&) {}
		}
	}

	node.fail("Invalid destination, use either " ANSI_bCYAN "main" ANSI_RESET
		" or " ANSI_bCYAN "ovXX" ANSI_RESET ".");
}

void readMode(RegionConfig& region, const cfg::Node& regionNode)
{
	const cfg::Node node = regionNode["mode"];
	if (!node.defined() || node.isNull())
		return;

	const std::string mode = node.asString();
	for (int i = 0; i <= int(RegionMode::Create); i++)
	{
		if (mode == regionModeName(RegionMode(i)))
		{
			region.mode = RegionMode(i);
			return;
		}
	}

	std::ostringstream oss;
	oss << "Invalid mode " << OSTR(mode) << ", expected " ANSI_bCYAN
	    "append" ANSI_RESET ", " ANSI_bCYAN "replace" ANSI_RESET " or " ANSI_bCYAN "create" ANSI_RESET ".";
	node.fail(oss.str());
}

void readOverwrites(RegionConfig& region, const cfg::Node& regionNode)
{
	const cfg::Node node = regionNode["overwrites"];
	if (!node.defined() || node.isNull())
		return;

	for (const cfg::Node& pair : node.items())
	{
		if (pair.size() != 2)
			pair.fail("Expected a " ANSI_bCYAN "[start, end]" ANSI_RESET " address pair.");

		Overwrite overwrite;
		overwrite.startAddress = pair[std::size_t(0)].asU32();
		overwrite.endAddress = pair[std::size_t(1)].asU32();

		if (overwrite.startAddress >= overwrite.endAddress)
		{
			std::ostringstream oss;
			oss << "Overwrite start address " << OSTR(pair[std::size_t(0)].asString())
			    << " must be below the end address " << OSTR(pair[std::size_t(1)].asString()) << ".";
			pair.fail(oss.str());
		}

		region.overwrites.push_back(overwrite);
	}
}

void readRegion(RegionConfig& region, const cfg::Node& node, const Expander& expander)
{
	if (!node.isMap())
		node.failType("a mapping");

	checkKeys(node, "a region", {
		"dest", "mode", "address", "maxsize", "compress",
		"sources", "defines", "flags", "overwrites" });

	region.mark = node.mark();
	readDestination(region, node.require("dest"));
	readMode(region, node);

	region.sources = readListOp(node["sources"], expander);
	region.defines = readListOp(node["defines"], expander);
	region.flags = readFlagOps(node["flags"], expander);
	region.compress = node["compress"].asBool(false);

	if (node.has("address"))
	{
		region.address.set(node["address"].asU32(), Source::RegionSection);
	}
	else if (region.mode == RegionMode::Create)
	{
		node.fail("A region in " ANSI_bCYAN "create" ANSI_RESET " mode needs an "
			ANSI_bCYAN "address" ANSI_RESET ".");
	}
	else if (region.mode == RegionMode::Replace)
	{
		// No address means "wherever there is room", which the patcher spells
		// as the all-ones sentinel.
		region.address.set(0xFFFFFFFFu, Source::Default);
	}

	if (node.has("maxsize"))
		region.maxsize.set(node["maxsize"].asU32(), Source::RegionSection);

	readOverwrites(region, node);
}

// The expander is taken by value: a target adds ${target.name} and
// ${target.build} to it, and arm9's must not be visible while arm7 is read.
void readTarget(TargetConfig& target, const cfg::Node& node, Expander expander,
                const fs::path& projectFile)
{
	target.file = projectFile;
	target.mark = node.mark();

	if (!node.isMap())
		node.failType("a mapping");

	checkKeys(node, "a target", {
		"enabled", "build", "workdir", "arena-lo", "symbols",
		"includes", "defines", "flags", "regions" });

	target.enabled = node["enabled"].asBool(true);
	if (!target.enabled)
		return;

	expander.setConstant("target.name", target.name);

	if (node.has("build"))
		target.buildDir.set(expander.expand(node["build"].asString(), node["build"]), Source::TargetSection);
	else
		node.fail("A target needs a " ANSI_bCYAN "build" ANSI_RESET " directory.");

	// Set before anything else in the target is expanded, so a region can say
	// ${target.build}/generated without depending on key order.
	expander.setConstant("target.build", target.buildDir.value.string());

	if (node.has("workdir"))
		target.workDir.set(expander.expand(node["workdir"].asString(), node["workdir"]), Source::TargetSection);

	if (node.has("symbols"))
	{
		fs::path symbols = expander.expand(node["symbols"].asString(), node["symbols"]);
		symbols.make_preferred();
		target.symbols.set(std::move(symbols), Source::TargetSection);
	}

	if (node.has("arena-lo"))
		target.arenaLo.set(node["arena-lo"].asU32(), Source::TargetSection);

	target.includes = readListOp(node["includes"], expander);
	target.defines = readListOp(node["defines"], expander);
	target.flags = readFlagOps(node["flags"], expander);

	const cfg::Node regions = node["regions"];
	if (!regions.defined() || regions.isNull())
		node.fail("A target needs at least one region.");

	for (const cfg::Node& regionNode : regions.items())
	{
		RegionConfig region;
		readRegion(region, regionNode, expander);
		target.regions.push_back(std::move(region));
	}

	if (target.regions.empty())
		regions.fail("A target needs at least one region.");
}

} // namespace

ProjectConfig loadV2(const fs::path& projectFile, const fs::path& projectRoot,
                     const VarOverrides& varOverrides)
{
	ProjectConfig config;
	config.version = 2;
	config.file = projectFile;
	config.projectRoot = projectRoot;

	const cfg::Document doc(projectFile);
	const cfg::Node root = doc.root();
	if (!root.isMap())
		root.failType("a mapping");

	checkKeys(root, "the project", {
		"version", "vars", "rom", "toolchain", "build",
		"includes", "defines", "flags", "pre-build", "post-build", "targets" });

	const cfg::Node version = root.require("version");
	if (version.asInt() != 2)
		version.fail("Unsupported configuration version; this NCPatcher reads version 2.");

	Expander expander;
	expander.setConstant("project.root", projectRoot.string());
	expander.setConstant("config.dir", projectFile.parent_path().string());

	const cfg::Node vars = root["vars"];
	if (vars.defined() && !vars.isNull())
	{
		if (!vars.isMap())
			vars.failType("a mapping of names to values");
		for (const auto& [name, value] : vars.fields())
			expander.setVariable(name, value.asString(), value);
	}

	for (const auto& [name, value] : varOverrides)
		expander.setOverride(name, value);

	// ROM location ---------------------------------------------------------

	const cfg::Node rom = root.require("rom");
	if (!rom.isMap())
		rom.failType("a mapping");
	checkKeys(rom, "rom", { "file", "dir", "output", "backup", "layout", "arm9-slack" });

	const bool hasFile = rom.has("file");
	const bool hasDir = rom.has("dir");
	if (hasFile && hasDir)
		rom.fail("Give either " ANSI_bCYAN "rom.file" ANSI_RESET " or " ANSI_bCYAN "rom.dir" ANSI_RESET ", not both.");
	if (!hasFile && !hasDir)
		rom.fail("Give either " ANSI_bCYAN "rom.file" ANSI_RESET " or " ANSI_bCYAN "rom.dir" ANSI_RESET ".");

	if (hasFile)
	{
		config.romFile.set(expander.expand(rom["file"].asString(), rom["file"]), Source::ProjectFile);
		expander.setConstant("rom.dir", config.romFile.value.parent_path().string());
	}
	else
	{
		config.filesystemDir.set(expander.expand(rom["dir"].asString(), rom["dir"]), Source::ProjectFile);
		expander.setConstant("rom.dir", config.filesystemDir.value.string());
	}

	config.backupDir.set(expander.expand(rom.require("backup").asString(), rom["backup"]), Source::ProjectFile);

	if (rom.has("output"))
	{
		if (!hasFile)
			rom["output"].fail("Only a project that patches a .nds can write one; give " ANSI_bCYAN "rom.file" ANSI_RESET " as well.");
		config.romOutput.set(expander.expand(rom["output"].asString(), rom["output"]), Source::ProjectFile);
	}

	if (rom.has("layout"))
	{
		const cfg::Node layout = rom["layout"];
		if (layout.isMap())
		{
			// A mapping names the files directly, optionally starting from a
			// preset. The keys are not checked here: which names exist is the
			// accessor's business, and it reports an unknown one against the
			// layout it was building.
			for (const auto& [key, value] : layout.fields())
			{
				if (key == "preset")
					config.romLayoutPreset.set(value.asString(), Source::ProjectFile);
				else
					config.romLayoutOverrides.emplace_back(key, expander.expand(value.asString(), value));
			}
		}
		else
		{
			config.romLayoutPreset.set(layout.asString(), Source::ProjectFile);
		}
	}

	if (rom.has("arm9-slack"))
	{
		// `auto` means "let ncpatcher choose", which is also what leaving the
		// key out means. Spelling it is allowed so that a project can say the
		// default is deliberate.
		const cfg::Node slack = rom["arm9-slack"];
		if (slack.asString("") != "auto")
			config.romArm9Slack.set(slack.asU32(), Source::ProjectFile);
	}

	// Toolchain and build --------------------------------------------------

	const cfg::Node toolchain = root["toolchain"];
	if (toolchain.defined() && !toolchain.isNull())
	{
		if (toolchain.isMap())
		{
			checkKeys(toolchain, "toolchain", { "prefix" });
			config.toolchain.set(
				expander.expand(toolchain.require("prefix").asString(), toolchain["prefix"]),
				Source::ProjectFile);
		}
		else
		{
			// `toolchain: arm-none-eabi-` reads the same and is what most
			// projects want to write.
			config.toolchain.set(expander.expand(toolchain.asString(), toolchain), Source::ProjectFile);
		}
	}
	else
	{
		config.toolchain.set("arm-none-eabi-", Source::Default);
	}

	const cfg::Node build = root["build"];
	if (build.defined() && !build.isNull())
	{
		if (!build.isMap())
			build.failType("a mapping");
		checkKeys(build, "build", { "threads" });
		if (build.has("threads"))
			config.threadCount.set(build["threads"].asInt(), Source::ProjectFile);
	}

	// Shared settings ------------------------------------------------------

	config.includes = readListOp(root["includes"], expander);
	config.defines = readListOp(root["defines"], expander);
	config.flags = readFlagOps(root["flags"], expander);
	config.preBuild = readStrings(root["pre-build"], expander);
	config.postBuild = readStrings(root["post-build"], expander);

	// Resolved copies of the declared vars. Nothing in the build reads these --
	// expansion already happened -- but `config dump` and `migrate` do.
	if (vars.defined() && !vars.isNull())
	{
		for (const auto& [name, value] : vars.fields())
			config.vars[name] = expander.expand(value.asString(), value);
	}

	// Last, so that what `config dump` shows is what the build actually used.
	for (const auto& [name, value] : varOverrides)
		config.vars[name] = value;

	// Targets --------------------------------------------------------------

	const cfg::Node targets = root.require("targets");
	if (!targets.isMap())
		targets.failType("a mapping of target names");
	checkKeys(targets, "targets", { "arm7", "arm9" });

	for (bool arm9 : { false, true })
	{
		TargetConfig& target = config.target(arm9);
		target.name = arm9 ? "arm9" : "arm7";
		target.arm9 = arm9;

		const cfg::Node node = targets[target.name];
		if (!node.defined() || node.isNull())
			continue;

		readTarget(target, node, expander, projectFile);
	}

	if (!config.arm7.enabled && !config.arm9.enabled)
		throw ncp::exception("No targets to build were specified.");

	return config;
}

} // namespace ncp::config
