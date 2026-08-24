#include "target_resolver.hpp"

#include <algorithm>
#include <sstream>

#include "../system/log.hpp"
#include "../system/except.hpp"
#include "../utils/glob.hpp"

namespace fs = std::filesystem;

namespace ncp::config {

namespace {

std::string join(const std::vector<std::string>& parts, char separator)
{
	std::string out;
	for (const std::string& part : parts)
	{
		if (part.empty())
			continue;
		if (!out.empty())
			out += separator;
		out += part;
	}
	return out;
}

// Expands one level's worth of patterns.
//
// A leading '!' excludes rather than includes, and exclusions are applied after
// everything has matched -- so the order of entries in the list does not change
// the outcome, which is what people expect from a list of paths and not what
// they would get from applying each entry in sequence.
std::vector<fs::path> expandPatterns(const std::vector<std::string>& patterns,
                                     const fs::path& baseDir, bool directoriesOnly, bool quiet)
{
	Glob::Options options;
	options.directoriesOnly = directoriesOnly;

	std::vector<fs::path> matched;
	std::vector<std::string> exclusions;

	for (const std::string& pattern : patterns)
	{
		if (pattern.starts_with('!'))
		{
			exclusions.push_back(pattern.substr(1));
			continue;
		}

		std::vector<fs::path> found = Glob::expand(pattern, baseDir, options);
		if (found.empty())
		{
			if (!quiet)
			{
				Log::out << OWARN << (Glob::hasWildcard(pattern)
					? "Pattern matched nothing: "
					: "Ignored non-existent path: ") << OSTR(pattern) << std::endl;
			}
			continue;
		}

		matched.insert(matched.end(), found.begin(), found.end());
	}

	if (exclusions.empty())
		return matched;

	std::vector<fs::path> out;
	out.reserve(matched.size());
	for (const fs::path& path : matched)
	{
		const std::string generic = path.generic_string();
		const bool excluded = std::any_of(exclusions.begin(), exclusions.end(),
			[&generic](const std::string& exclusion) { return Glob::match(exclusion, generic); });
		if (!excluded)
			out.push_back(path);
	}

	return out;
}

// "NAME=VALUE" and "NAME" both name NAME.
std::string_view defineName(std::string_view entry)
{
	const std::size_t equals = entry.find('=');
	return equals == std::string_view::npos ? entry : entry.substr(0, equals);
}

void applyDefines(DefineSet& defines, const ListOp& op, const char* origin)
{
	if (op.hasSet)
	{
		defines = DefineSet();
		for (const std::string& entry : op.set)
			defines.add(entry, origin, true);
	}

	for (const std::string& entry : op.remove)
		defines.remove(defineName(entry));

	for (const std::string& entry : op.append)
	{
		// A level redefining a name it set itself is a mistake worth reporting;
		// a level overriding one it inherited is the point of inheritance.
		const DefineSet::Define* existing = defines.find(defineName(entry));
		const bool sameLevel = existing != nullptr && existing->origin == origin;
		defines.add(entry, origin, sameLevel);
	}
}

void describeList(std::ostringstream& oss, const char* name, const std::vector<std::string>& values)
{
	oss << name << '=' << join(values, '\x1f') << '\n';
}

} // namespace

BuildTarget TargetResolver::resolve(const ProjectConfig& config, const TargetConfig& target,
                                    const PathContext& paths, const Options& options)
{
	BuildTarget out;
	BuildTargetBuilder::setArm9(out, target.arm9);

	const FlagLists projectFlags = FlagLists().inheritedBy(config.flags);
	const FlagLists targetFlags = projectFlags.inheritedBy(target.flags);

	DefineSet projectDefines;
	applyDefines(projectDefines, config.defines, "the project");
	DefineSet targetDefines = projectDefines;
	applyDefines(targetDefines, target.defines, "the target");

	out.arenaLo = int(target.arenaLo.value);
	out.symbols = target.symbols.value;

	const std::vector<std::string> targetDefineFlags = targetDefines.toFlags();
	auto joinFlags = [](const std::vector<std::string>& common,
	                    const std::vector<std::string>& specific,
	                    const std::vector<std::string>& defines) {
		std::vector<std::string> all;
		all.reserve(common.size() + specific.size() + defines.size());
		all.insert(all.end(), common.begin(), common.end());
		all.insert(all.end(), specific.begin(), specific.end());
		all.insert(all.end(), defines.begin(), defines.end());
		return join(all, ' ');
	};

	out.cFlags = joinFlags(targetFlags.common, targetFlags.c, targetDefineFlags);
	out.cppFlags = joinFlags(targetFlags.common, targetFlags.cpp, targetDefineFlags);
	out.asmFlags = joinFlags(targetFlags.common, targetFlags.asm_, targetDefineFlags);

	// Comma-joined because it is handed to gcc as one -Wl, argument. v1 wrote
	// the commas by hand and the reader splits on them, so the two schemas
	// produce the same string.
	out.ldFlags = join(targetFlags.ld, ',');

	const std::vector<std::string> includePatterns =
		target.includes.applyTo(config.includes.applyTo({}));
	out.includes = expandPatterns(includePatterns, paths.targetWorkDir, true, options.quiet);


	for (const RegionConfig& regionConfig : target.regions)
	{
		BuildTarget::Region region;
		region.destination = regionConfig.destination;
		region.mode = BuildTarget::Mode(regionConfig.mode);
		region.compress = regionConfig.compress;
		region.address = regionConfig.address.value;
		region.maxsize = int(regionConfig.maxsize.configured()
			? regionConfig.maxsize.value : 0x100000u);

		region.overwrites.reserve(regionConfig.overwrites.size());
		for (const Overwrite& overwrite : regionConfig.overwrites)
			region.overwrites.push_back({ overwrite.startAddress, overwrite.endAddress });

		DefineSet regionDefines = targetDefines;
		applyDefines(regionDefines, regionConfig.defines, "the region");

		const FlagLists regionFlags = targetFlags.inheritedBy(regionConfig.flags);
		const std::vector<std::string> regionDefineFlags = regionDefines.toFlags();
		region.cFlags = joinFlags(regionFlags.common, regionFlags.c, regionDefineFlags);
		region.cppFlags = joinFlags(regionFlags.common, regionFlags.cpp, regionDefineFlags);
		region.asmFlags = joinFlags(regionFlags.common, regionFlags.asm_, regionDefineFlags);

		region.sources = expandPatterns(regionConfig.sources.applyTo({}), paths.targetWorkDir, false, options.quiet);

		if (region.compress && region.destination < 0)
		{
			// Only overlays are compressible: the ARM9 binary's compression is
			// described by ModuleParams and undone on load, and re-doing it
			// would mean rewriting the autoload machinery this tool patches.
			Log::out << OWARN << OSTRa("compress") << " applies to overlays; region "
			         << OSTR(regionConfig.dest) << " will be written uncompressed." << std::endl;
			region.compress = false;
		}

		out.regions.push_back(std::move(region));
	}

	return out;
}

std::string TargetResolver::describe(const ProjectConfig& config, const BuildTarget& resolved)
{
	std::ostringstream oss;

	oss << "target=" << (resolved.getArm9() ? "arm9" : "arm7") << '\n';
	oss << "toolchain=" << config.toolchain.value << '\n';
	oss << "arenaLo=" << resolved.arenaLo << '\n';
	oss << "symbols=" << resolved.symbols.generic_string() << '\n';
	oss << "cFlags=" << resolved.cFlags << '\n';
	oss << "cppFlags=" << resolved.cppFlags << '\n';
	oss << "asmFlags=" << resolved.asmFlags << '\n';
	oss << "ldFlags=" << resolved.ldFlags << '\n';

	std::vector<std::string> includes;
	includes.reserve(resolved.includes.size());
	for (const fs::path& include : resolved.includes)
		includes.push_back(include.generic_string());
	describeList(oss, "includes", includes);

	for (const BuildTarget::Region& region : resolved.regions)
	{
		oss << "region " << region.destination
		    << " mode=" << int(region.mode)
		    << " address=" << region.address
		    << " maxsize=" << region.maxsize
		    << " compress=" << int(region.compress) << '\n';
		oss << "  c=" << region.cFlags << '\n';
		oss << "  cpp=" << region.cppFlags << '\n';
		oss << "  asm=" << region.asmFlags << '\n';
		for (const BuildTarget::Overwrites& overwrite : region.overwrites)
			oss << "  overwrite=" << overwrite.startAddress << ':' << overwrite.endAddress << '\n';
	}

	return oss.str();
}

std::string TargetResolver::describeProject(const ProjectConfig& config,
                                            const std::vector<std::string>& commandLineDefines)
{
	std::ostringstream oss;

	oss << "toolchain=" << config.toolchain.value << '\n';
	oss << "backup=" << config.backupDir.value.generic_string() << '\n';
	oss << "filesystem=" << config.filesystemDir.value.generic_string() << '\n';
	oss << "rom=" << config.romFile.value.generic_string() << '\n';

	// The command-line defines belong here rather than in a target: they reach
	// every compiler invocation, and changing them has to invalidate every
	// object that was built without them.
	describeList(oss, "cli-defines", commandLineDefines);

	return oss.str();
}

} // namespace ncp::config
