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

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <set>
#include <sstream>

#include "expander.hpp"
#include "../system/log.hpp"
#include "../system/except.hpp"

namespace fs = std::filesystem;

namespace ncp::config {

namespace {

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
		oss << "Unknown key " << OSTR(key) << " in " << what << "." OREASONNL "Expected one of: ";
		bool first = true;
		for (std::string_view candidate : known)
		{
			oss << (first ? "" : ", ") << candidate;
			first = false;
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

HookWhen readHookWhen(const cfg::Node& node, const Expander& expander)
{
	const std::string value = expander.expand(node.asString(), node);
	if (value == "pre-build")
		return HookWhen::PreBuild;
	if (value == "post-build")
		return HookWhen::PostBuild;
	node.fail("Invalid hook phase; expected " ANSI_bCYAN "pre-build" ANSI_RESET
		" or " ANSI_bCYAN "post-build" ANSI_RESET ".");
}

std::vector<HookConfig> readHooks(const cfg::Node& node, const Expander& expander)
{
	std::vector<HookConfig> out;
	if (!node.defined() || node.isNull())
		return out;
	if (!node.isSequence())
		node.failType("a list of hooks");

	for (const cfg::Node& item : node.items())
	{
		if (!item.isMap())
			item.failType("a hook mapping");
		checkKeys(item, "a hook", { "name", "run", "cwd", "env", "when" });

		HookConfig hook;
		hook.name = expander.expand(item.require("name").asString(), item["name"]);
		hook.run = expander.expand(item.require("run").asString(), item["run"]);
		hook.when = readHookWhen(item.require("when"), expander);
		if (hook.name.empty())
			item["name"].fail("A hook name cannot be empty.");
		if (hook.run.empty())
			item["run"].fail("A hook command cannot be empty.");

		if (item.has("cwd"))
			hook.cwd = expander.expand(item["cwd"].asString(), item["cwd"]);

		const cfg::Node env = item["env"];
		if (env.defined() && !env.isNull())
		{
			if (!env.isMap())
				env.failType("a mapping of environment variables");
			for (const auto& [name, value] : env.fields())
			{
				if (name.empty() || (!std::isalpha(static_cast<unsigned char>(name[0])) && name[0] != '_')
					|| name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_", 1)
						!= std::string::npos)
					value.fail("Invalid environment variable name \"" + name + "\".");
				hook.env.emplace_back(name, expander.expand(value.asString(), value));
			}
		}

		out.push_back(std::move(hook));
	}

	return out;
}

void appendLegacyHooks(std::vector<HookConfig>& out, const cfg::Node& node,
	                   HookWhen when, const Expander& expander)
{
	const std::vector<std::string> commands = readStrings(node, expander);
	for (std::size_t i = 0; i < commands.size(); i++)
	{
		HookConfig hook;
		hook.name = std::string(hookWhenName(when)) + " #" + std::to_string(i + 1);
		hook.run = commands[i];
		hook.when = when;
		out.push_back(std::move(hook));
	}
}

void validateNitroPath(const std::string& path, const cfg::Node& node)
{
	if (path.empty() || path.front() == '/' || path.back() == '/' || path.find('\\') != std::string::npos)
		node.fail("A NitroFS path must be a non-empty relative path using '/' separators.");
	if (path == "z_new/reserved")
		node.fail(ANSI_bCYAN "z_new/reserved" ANSI_RESET " is managed by NCPatcher.");

	std::size_t start = 0;
	while (start < path.size())
	{
		const std::size_t slash = path.find('/', start);
		const std::size_t end = slash == std::string::npos ? path.size() : slash;
		const std::string_view part(path.data() + start, end - start);
		if (part.empty() || part == "." || part == "..")
			node.fail("A NitroFS path cannot contain empty, '.' or '..' segments.");
		if (part.size() > 0x7F)
			node.fail("A NitroFS path segment cannot exceed 127 bytes.");
		if (slash == std::string::npos)
			break;
		start = slash + 1;
	}
}

std::vector<FileConfig> readFiles(const cfg::Node& node, const Expander& expander)
{
	std::vector<FileConfig> out;
	if (!node.defined() || node.isNull())
		return out;
	if (!node.isMap())
		node.failType("a mapping of NitroFS paths to source files");

	for (const auto& [rawPath, source] : node.fields())
	{
		FileConfig file;
		file.path = expander.expand(rawPath, source);
		validateNitroPath(file.path, source);
		file.source = expander.expand(source.asString(), source);
		if (file.source.empty())
			source.fail("A NitroFS source path cannot be empty.");

		const auto duplicate = std::find_if(out.begin(), out.end(),
			[&](const FileConfig& previous) { return previous.path == file.path; });
		if (duplicate != out.end())
			source.fail("NitroFS path \"" + file.path + "\" is configured more than once.");

		out.push_back(std::move(file));
	}
	return out;
}

std::vector<VariantConfig> readVariants(const cfg::Node& node, const Expander& expander)
{
	std::vector<VariantConfig> out;
	if (!node.defined() || node.isNull())
		return out;
	if (!node.isMap())
		node.failType("a mapping of named variants");

	for (const auto& [rawName, body] : node.fields())
	{
		if (!body.isMap())
			body.failType("a variant mapping");
		checkKeys(body, "a variant", { "defines", "files" });

		VariantConfig variant;
		variant.name = expander.expand(rawName, body);
		if (variant.name.empty()
			|| !std::isalnum(static_cast<unsigned char>(variant.name.front()))
			|| variant.name.find_first_not_of(
				"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-") != std::string::npos)
			body.fail("A variant name must start with a letter or digit and contain only letters, digits, '_', '-' or '.'.");

		std::string folded = variant.name;
		std::transform(folded.begin(), folded.end(), folded.begin(),
			[](unsigned char c) { return char(std::tolower(c)); });
		const auto duplicate = std::find_if(out.begin(), out.end(), [&](const VariantConfig& previous) {
			std::string previousFolded = previous.name;
			std::transform(previousFolded.begin(), previousFolded.end(), previousFolded.begin(),
				[](unsigned char c) { return char(std::tolower(c)); });
			return previousFolded == folded;
		});
		if (duplicate != out.end())
			body.fail("Variant name \"" + variant.name + "\" is configured more than once.");

		variant.defines = readStrings(body["defines"], expander);
		variant.files = readFiles(body["files"], expander);
		out.push_back(std::move(variant));
	}

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

// Reads the overlay catalog a target points at.
//
// The catalog is game knowledge, not project configuration: it says which
// overlays the game has and how far each one may grow before it runs into
// whatever the game put after it. One table serves every project built against
// that game, which is why it lives outside the project and is referenced
// rather than copied -- a copied table goes stale silently, and a stale ceiling
// is an overlay that overruns its neighbour.
//
// Only the fields that describe the overlay itself are accepted here. Sources,
// flags and defines are the project's business, so the catalog cannot smuggle
// them in.
std::vector<RegionConfig> readRegionCatalog(const fs::path& file, const cfg::Node& where)
{
	std::vector<RegionConfig> entries;
	if (!fs::exists(file))
	{
		std::ostringstream oss;
		oss << "The region catalog " << OSTR(file.string()) << " does not exist.";
		where.fail(oss.str());
	}

	const cfg::Document doc(file);
	const cfg::Node root = doc.root();
	if (!root.isMap())
		root.failType("a mapping");

	checkKeys(root, "the region catalog", { "version", "regions" });

	const cfg::Node version = root.require("version");
	if (version.asInt() != 1)
		version.fail("Unsupported region catalog version; this NCPatcher reads version 1.");

	const cfg::Node regions = root.require("regions");
	if (!regions.isSequence())
		regions.failType("a list of regions");

	std::set<int> seen;
	for (const cfg::Node& regionNode : regions.items())
	{
		if (!regionNode.isMap())
			regionNode.failType("a mapping");

		checkKeys(regionNode, "a catalog entry", { "dest", "address", "maxsize", "compress" });

		RegionConfig region;
		region.fromCatalog = true;
		region.mark = regionNode.mark();
		readDestination(region, regionNode.require("dest"));

		if (!seen.insert(region.destination).second)
		{
			std::ostringstream oss;
			oss << "The region catalog lists " << OSTR(region.dest) << " more than once.";
			regionNode.fail(oss.str());
		}

		if (regionNode.has("address"))
			region.address.set(regionNode["address"].asU32(), Source::RegionSection);
		if (regionNode.has("maxsize"))
			region.maxsize.set(regionNode["maxsize"].asU32(), Source::RegionSection);
		region.compress = regionNode["compress"].asBool(false);

		entries.push_back(std::move(region));
	}

	return entries;
}

// Lays one of the target's own regions over the catalog.
//
// The target always wins where it says something. Where it stays silent the
// catalog's value survives, which is the whole point: a project names an
// overlay to put sources in it, not to restate a size limit it has no opinion
// about.
void mergeRegion(TargetConfig& target, RegionConfig&& region, const cfg::Node& node)
{
	const auto existing = std::find_if(target.regions.begin(), target.regions.end(),
		[&](const RegionConfig& candidate) { return candidate.destination == region.destination; });

	if (existing != target.regions.end())
	{
		std::ostringstream oss;
		oss << "Region " << OSTR(region.dest) << " is declared more than once.";
		node.fail(oss.str());
	}

	target.regions.push_back(std::move(region));
}

// Lays the catalog under the regions the target wrote out itself.
//
// The target always wins where it says something. Where it stays silent the
// catalog's value survives, which is the whole point: a project names an
// overlay to put sources in it, not to restate a size limit it has no opinion
// about. Entries the target never mentioned are appended, in catalog order, so
// the regions a project actually wrote stay where it put them.
void applyCatalog(TargetConfig& target, std::vector<RegionConfig>&& catalog)
{
	for (RegionConfig& entry : catalog)
	{
		const auto declared = std::find_if(target.regions.begin(), target.regions.end(),
			[&](const RegionConfig& candidate) { return candidate.destination == entry.destination; });

		if (declared == target.regions.end())
		{
			target.regions.push_back(std::move(entry));
			continue;
		}

		if (!declared->address.configured() && entry.address.configured())
			declared->address = entry.address;
		if (!declared->maxsize.configured() && entry.maxsize.configured())
			declared->maxsize = entry.maxsize;
	}
}

// The project's half of the module system: which modules, and what the project
// wants changed about them.
//
// Nothing here reads a module.yaml -- that happens later, once the working
// directory is settled and the graph is built, because `modules dump` has to
// work without a toolchain and a build has to write the dump before its
// pre-build commands run.
void readComponentOverride(ComponentOverride& override_, const cfg::Node& node)
{
	override_.location = node.location();

	if (node.isScalar() && !node.isNull())
	{
		// `SomeComponent: false` -- the short form, and by far the common one.
		override_.hasEnabled = true;
		override_.enabled = node.asBool();
		return;
	}

	if (!node.isMap())
		node.failType("a boolean or a mapping");

	checkKeys(node, "a component override", { "enabled", "target", "defines" });

	if (node.has("enabled"))
	{
		override_.hasEnabled = true;
		override_.enabled = node["enabled"].asBool();
	}

	if (node.has("target"))
		override_.target = node["target"].asString();

	const cfg::Node defines = node["defines"];
	if (defines.defined() && !defines.isNull())
	{
		if (!defines.isMap())
			defines.failType("a mapping of define names to values");
		for (const auto& [name, value] : defines.fields())
			override_.defines.emplace_back(name, value.asString());
	}
}

void readModuleSelection(ModuleSelection& selection, const cfg::Node& node)
{
	selection.location = node.location();

	// `- coop` on its own: enabled, nothing overridden.
	if (node.isScalar())
	{
		selection.key = node.asString();
		return;
	}

	if (!node.isMap() || node.fields().size() != 1)
	{
		node.fail("Expected either a module name or one name with its settings under it, as "
			ANSI_bCYAN "- coop: { components: ... }" ANSI_RESET ".");
	}

	const auto [key, body] = node.fields().front();
	selection.key = key;
	selection.location = body.defined() ? body.location() : node.location();

	if (!body.defined() || body.isNull())
		return;

	if (body.isScalar())
	{
		// `- debug: false`
		selection.enabled = body.asBool();
		return;
	}

	if (!body.isMap())
		body.failType("a boolean or a mapping");

	checkKeys(body, "a module selection", { "enabled", "optional", "components" });
	selection.enabled = body["enabled"].asBool(true);
	selection.optional = body["optional"].asBool(false);

	const cfg::Node components = body["components"];
	if (!components.defined() || components.isNull())
		return;

	if (!components.isMap())
		components.failType("a mapping of component names");

	for (const auto& [name, value] : components.fields())
	{
		ComponentOverride override_;
		override_.name = name;
		readComponentOverride(override_, value);
		selection.components.push_back(std::move(override_));
	}
}

void readModules(ModulesConfig& modules, const cfg::Node& node, const Expander& expander)
{
	modules.present = true;

	if (!node.isMap())
		node.failType("a mapping");

	checkKeys(node, "modules", { "dir", "dump", "enabled" });

	modules.dir.set(node.has("dir")
		? fs::path(expander.expand(node["dir"].asString(), node["dir"]))
		: fs::path("modules"),
		node.has("dir") ? Source::ProjectFile : Source::Default);

	if (node.has("dump"))
		modules.dump.set(expander.expand(node["dump"].asString(), node["dump"]), Source::ProjectFile);

	const cfg::Node enabled = node["enabled"];
	if (!enabled.defined() || enabled.isNull())
		return;

	if (!enabled.isSequence())
		enabled.failType("a list of module names");

	for (const cfg::Node& item : enabled.items())
	{
		ModuleSelection selection;
		readModuleSelection(selection, item);

		if (selection.key.empty())
			item.fail("A module entry needs a name.");

		for (const ModuleSelection& previous : modules.selections)
		{
			if (previous.key != selection.key)
				continue;
			std::ostringstream oss;
			oss << "Module " << OSTR(selection.key) << " is listed more than once.";
			item.fail(oss.str());
		}

		modules.selections.push_back(std::move(selection));
	}
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
		"includes", "defines", "flags", "region-catalog", "regions" });

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

	std::vector<RegionConfig> catalogEntries;
	if (node.has("region-catalog"))
	{
		fs::path catalog = expander.expand(node["region-catalog"].asString(), node["region-catalog"]);
		if (catalog.is_relative())
			catalog = projectFile.parent_path() / catalog;
		catalog = catalog.lexically_normal();
		catalog.make_preferred();
		target.regionCatalog.set(catalog, Source::TargetSection);
		catalogEntries = readRegionCatalog(catalog, node["region-catalog"]);
	}

	const cfg::Node regions = node["regions"];
	if ((!regions.defined() || regions.isNull()) && catalogEntries.empty())
		node.fail("A target needs at least one region, or a "
			ANSI_bCYAN "region-catalog" ANSI_RESET " to take them from.");

	for (const cfg::Node& regionNode : regions.items())
	{
		RegionConfig region;
		readRegion(region, regionNode, expander);
		mergeRegion(target, std::move(region), regionNode);
	}

	applyCatalog(target, std::move(catalogEntries));

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
		"version", "vars", "rom", "toolchain", "build", "modules",
		"includes", "defines", "flags", "hooks", "files", "variants",
		"pre-build", "post-build", "targets" });

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
	const cfg::Node modules = root["modules"];
	if (modules.defined() && !modules.isNull())
		readModules(config.modules, modules, expander);

	if (config.modules.dump.configured())
	{
		const fs::path dump = config.modules.dump.value.is_absolute()
			? config.modules.dump.value
			: projectRoot / config.modules.dump.value;
		expander.setConstant("ncp.moduleDump", dump.lexically_normal().string());
	}
	config.files = readFiles(root["files"], expander);
	config.variants = readVariants(root["variants"], expander);

	if (root.has("hooks") && (root.has("pre-build") || root.has("post-build")))
		root["hooks"].fail("Use either " ANSI_bCYAN "hooks" ANSI_RESET
			" or the compatibility " ANSI_bCYAN "pre-build/post-build" ANSI_RESET
			" keys, not both.");
	if (root.has("hooks"))
	{
		config.hooks = readHooks(root["hooks"], expander);
	}
	else
	{
		appendLegacyHooks(config.hooks, root["pre-build"], HookWhen::PreBuild, expander);
		appendLegacyHooks(config.hooks, root["post-build"], HookWhen::PostBuild, expander);
	}

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
