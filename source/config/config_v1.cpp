// The v1 reader: ncpatcher.json plus a separate JSON per target.
//
// This is a translation, not a reimplementation. Every project that exists
// today is written in this schema, so what it accepts and what it produces must
// match what the previous release did, down to the whitespace of an assembled
// compiler command line. That is why flag strings stay whole here instead of
// being tokenised: joining tokens back together would normalise spacing that
// nobody asked to have normalised.
//
// Three deliberate departures, all of them cases where matching the old
// behaviour would mean keeping a silent failure:
//
//   * `length` is accepted again as a spelling of `maxsize`. It was renamed
//     without an alias, so every project still saying `length` silently lost
//     its size limit and fell back to the 1 MiB default.
//   * the [path, recursive] pair form of `includes`/`sources` is accepted;
//     the glob rewrite would throw on it, and six shipped projects use it.

#include "config_loader.hpp"

#include <cstdlib>
#include <sstream>
#include <unordered_map>

#include "../system/log.hpp"
#include "../system/except.hpp"

namespace fs = std::filesystem;

namespace ncp::config {

namespace {

// v1's three-syntax string templating, kept exactly as it behaved.
//
//   ${x}      a variable declared in this file
//   $${x}     a variable declared in the project file (target files only)
//   ${env:X}  an environment variable
//
// v2 replaces all three with one namespaced form; see Expander. Nothing here
// is worth carrying forward, but everything here is worth reproducing.
class V1Vars
{
public:
	V1Vars(std::string_view what, const V1Vars* parent, const V1Options& options) :
		m_what(what), m_parent(parent), m_options(options) {}

	void define(std::string name, std::string value)
	{
		// emplace, not assignment: a repeated $name keeps the first, which is
		// what the previous implementation did.
		m_vars.emplace(std::move(name), std::move(value));
	}

	[[nodiscard]] const std::string& get(const std::string& name, const cfg::Node& where) const
	{
		const auto it = m_vars.find(name);
		if (it == m_vars.end())
		{
			std::ostringstream oss;
			oss << "Could not find variable " << OSTR(name) << " in " << m_what << ".";
			where.fail(oss.str());
		}
		return it->second;
	}

	[[nodiscard]] std::string expand(std::string text, const cfg::Node& where) const
	{
		auto invalid = [&]() {
			std::ostringstream oss;
			oss << "Invalid variable template expansion in string " << OSTR(text)
			    << " in " << m_what << ".";
			where.fail(oss.str());
		};

		std::size_t pos = 0;
		while ((pos = text.find('$', pos)) != std::string::npos)
		{
			const bool parentScope = pos + 1 < text.size() && text[pos + 1] == '$';
			const std::size_t off = parentScope ? 1 : 0;

			if (pos + off + 4 > text.size())
				break;
			if (text[pos + off + 1] != '{')
				invalid();

			const std::size_t endpos = text.find('}', pos + off + 1);
			if (endpos == std::string::npos)
				break;

			const std::string name = text.substr(pos + off + 2, endpos - (pos + off + 2));
			const bool isEnv = name.starts_with("env:");
			if (parentScope && isEnv)
				invalid();

			std::string value;
			if (isEnv)
			{
				if (name.size() == 4)
					invalid();
				const std::string envName = name.substr(4);
				if (m_options.keepEnvironmentReferences)
				{
					// Carried through as the v2 spelling, so the emitted config
					// still asks the environment rather than answering for it.
					const std::string carried = "${env." + envName + "}";
					text.replace(pos, endpos - pos + 1, carried);
					pos += carried.size();
					continue;
				}
				const char* envValue = std::getenv(envName.c_str());
				if (envValue == nullptr)
				{
					std::ostringstream oss;
					oss << "Could not find environment variable " << OSTR(envName)
					    << " referenced in " << m_what << ".";
					where.fail(oss.str());
				}
				value = envValue;
			}
			else if (parentScope)
			{
				if (m_parent == nullptr)
					invalid();
				value = m_parent->get(name, where);
			}
			else
			{
				value = get(name, where);
			}

			text.replace(pos, endpos - pos + 1, value);
			pos += value.size();
		}

		return text;
	}

	[[nodiscard]] const std::unordered_map<std::string, std::string>& all() const { return m_vars; }
	[[nodiscard]] const V1Options& options() const { return m_options; }

private:
	std::string m_what;
	const V1Vars* m_parent;
	V1Options m_options;
	std::unordered_map<std::string, std::string> m_vars;
};

std::string readString(const cfg::Node& node, const V1Vars& vars)
{
	return vars.expand(node.asString(), node);
}

// Collects the `$name` members at the top of a v1 document, in document order,
// expanding each against the ones already seen.
void collectVars(const cfg::Node& root, V1Vars& vars)
{
	for (const auto& [name, value] : root.fields())
	{
		if (name.size() > 1 && name[0] == '$')
			vars.define(name.substr(1), readString(value, vars));
	}
}

ListOp readPatternList(const cfg::Node& node, bool directoriesOnly, const V1Vars& vars)
{
	ListOp out;
	if (!node.defined() || node.isNull())
		return out;

	for (const cfg::Node& entry : node.items())
	{
		if (entry.isSequence())
		{
			if (entry.size() != 2)
			{
				entry.fail("Expected a string pattern or a "
					ANSI_bCYAN "[path, recursive]" ANSI_RESET " pair.");
			}
			const std::string path = readString(entry[std::size_t(0)], vars);
			const bool recursive = entry[std::size_t(1)].asBool();
			legacyPathPairToPatterns(path, recursive, directoriesOnly, out.append);
			continue;
		}

		out.append.push_back(readString(entry, vars));
	}

	return out;
}

// v1 flag strings are opaque: one entry, joined back with nothing added.
ListOp readFlagString(const cfg::Node& node, const V1Vars& vars)
{
	ListOp out;
	if (!node.defined() || node.isNull())
		return out;

	std::string value = readString(node, vars);
	if (value.empty())
		return out;

	out.hasSet = true;
	out.set.push_back(std::move(value));
	return out;
}

// ld_flags is the one v1 flag string with internal structure: it is already
// comma-separated for -Wl. Splitting on commas and rejoining on commas is
// exactly the identity, and it lets the v2 list form share one code path.
ListOp readLdFlagString(const cfg::Node& node, const V1Vars& vars)
{
	ListOp out;
	if (!node.defined() || node.isNull())
		return out;

	const std::string value = readString(node, vars);
	if (value.empty())
		return out;

	out.hasSet = true;
	std::size_t start = 0;
	while (true)
	{
		const std::size_t comma = value.find(',', start);
		if (comma == std::string::npos)
		{
			out.set.push_back(value.substr(start));
			break;
		}
		out.set.push_back(value.substr(start, comma - start));
		start = comma + 1;
	}
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
	const cfg::Node modeNode = regionNode["mode"];
	if (!modeNode.defined() || modeNode.isNull())
	{
		region.mode = RegionMode::Append;
		return;
	}

	const std::string mode = modeNode.asString();
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
	modeNode.fail(oss.str());
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

		if (overwrite.startAddress == overwrite.endAddress)
		{
			std::ostringstream oss;
			oss << "Overwrite start address " << OSTR(pair[std::size_t(0)].asString())
			    << " must not be the same as the end address.";
			pair.fail(oss.str());
		}
		if (overwrite.startAddress > overwrite.endAddress)
		{
			std::ostringstream oss;
			oss << "Overwrite start address " << OSTR(pair[std::size_t(0)].asString())
			    << " must not be higher than the end address "
			    << OSTR(pair[std::size_t(1)].asString()) << ".";
			pair.fail(oss.str());
		}

		region.overwrites.push_back(overwrite);
	}
}

// The size limit, under either of its two spellings.
void readMaxsize(RegionConfig& region, const cfg::Node& regionNode, const V1Options& options)
{
	if (regionNode.has("maxsize"))
	{
		region.maxsize.set(regionNode["maxsize"].asU32(), Source::RegionSection);
		return;
	}

	if (regionNode.has("length"))
	{
		// Renaming this key without keeping the alias made every project that
		// still says `length` fall back to the 1 MiB default, so an overlay
		// could grow past the space reserved for it and nothing would say so.
		region.maxsize.set(regionNode["length"].asU32(), Source::RegionSection);
		if (!options.quiet)
		{
			Log::out << OWARN << OSTR(regionNode["length"].path())
			         << " is the old spelling of " << OSTRa("maxsize") << "; it still works."
			         << std::endl;
		}
	}
}

void readTargetFile(TargetConfig& target, const fs::path& file,
                    const V1Vars& projectVars)
{
	target.file = file;

	const cfg::Document doc(file);
	const cfg::Node root = doc.root();
	target.mark = root.mark();

	if (!root.isMap())
		root.failType("a mapping");

	V1Vars vars(target.arm9 ? "the \"arm9\" target" : "the \"arm7\" target",
		&projectVars, projectVars.options());
	collectVars(root, vars);

	if (root.has("arenaLo"))
		target.arenaLo.set(root["arenaLo"].asU32(), Source::TargetSection);

	if (root.has("symbols"))
	{
		fs::path symbols = readString(root["symbols"], vars);
		symbols.make_preferred();
		target.symbols.set(std::move(symbols), Source::TargetSection);
	}

	target.includes = readPatternList(root.require("includes"), true, vars);

	target.flags.c = readFlagString(root.require("c_flags"), vars);
	target.flags.cpp = readFlagString(root.require("cpp_flags"), vars);
	target.flags.asm_ = readFlagString(root.require("asm_flags"), vars);
	target.flags.ld = readLdFlagString(root.require("ld_flags"), vars);

	for (const cfg::Node& regionNode : root.require("regions").items())
	{
		if (!regionNode.isMap())
			regionNode.failType("a mapping");

		RegionConfig region;
		region.mark = regionNode.mark();
		region.sources = readPatternList(regionNode.require("sources"), false, vars);
		readDestination(region, regionNode.require("dest"));
		region.compress = regionNode["compress"].asBool(false);
		readMode(region, regionNode);

		if (regionNode.has("c_flags"))
			region.flags.c = readFlagString(regionNode["c_flags"], vars);
		if (regionNode.has("cpp_flags"))
			region.flags.cpp = readFlagString(regionNode["cpp_flags"], vars);
		if (regionNode.has("asm_flags"))
			region.flags.asm_ = readFlagString(regionNode["asm_flags"], vars);

		if (region.mode == RegionMode::Replace)
		{
			// No address means "wherever there is room", which the patcher
			// spells as the all-ones sentinel.
			if (regionNode.has("address"))
				region.address.set(regionNode["address"].asU32(), Source::RegionSection);
			else
				region.address.set(0xFFFFFFFFu, Source::Default);
		}
		else if (region.mode == RegionMode::Create)
		{
			region.address.set(regionNode.require("address").asU32(), Source::RegionSection);
		}

		readMaxsize(region, regionNode, vars.options());
		readOverwrites(region, regionNode);

		target.regions.push_back(std::move(region));
	}
}

} // namespace

void legacyPathPairToPatterns(const std::string& rawPath, bool recursive, bool directoriesOnly,
                              std::vector<std::string>& out)
{
	std::string path = rawPath;
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

ProjectConfig loadV1(const fs::path& projectFile, const fs::path& projectRoot,
                     const V1Options& options)
{
	ProjectConfig config;
	config.version = 1;
	config.file = projectFile;
	config.projectRoot = projectRoot;

	const cfg::Document doc(projectFile);
	const cfg::Node root = doc.root();
	if (!root.isMap())
		root.failType("a mapping");

	V1Vars vars("\"" + projectFile.filename().string() + "\"", nullptr, options);
	vars.define("root", projectRoot.string());

	// Before the file's own, because define() keeps the first value it is
	// given: that is how a repeated $name behaved in v1, and it is what makes
	// --var an override rather than a suggestion.
	for (const auto& [name, value] : options.varOverrides)
		vars.define(name, value);

	collectVars(root, vars);
	config.vars = vars.all();

	config.backupDir.set(readString(root.require("backup"), vars), Source::ProjectFile);
	config.filesystemDir.set(readString(root.require("filesystem"), vars), Source::ProjectFile);
	config.toolchain.set(readString(root.require("toolchain"), vars), Source::ProjectFile);
	config.threadCount.set(root.require("thread-count").asInt(), Source::ProjectFile);

	for (const cfg::Node& command : root.require("pre-build").items())
		config.preBuild.push_back(readString(command, vars));
	for (const cfg::Node& command : root.require("post-build").items())
		config.postBuild.push_back(readString(command, vars));

	for (bool arm9 : { false, true })
	{
		TargetConfig& target = config.target(arm9);
		target.name = arm9 ? "arm9" : "arm7";
		target.arm9 = arm9;

		const cfg::Node node = root[target.name];
		// An empty object is how v1 spells "this target is not built"; several
		// projects carry `"arm7": {}` for exactly that.
		if (!node.defined() || node.isNull() || !node.isMap() || node.size() == 0)
			continue;

		target.enabled = true;
		target.buildDir.set(readString(node.require("build"), vars), Source::ProjectFile);

		if (node.has("workdir"))
			target.workDir.set(readString(node["workdir"], vars), Source::ProjectFile);

		const fs::path targetFile = readString(node.require("target"), vars);
		target.file = targetFile.is_absolute() ? targetFile : projectRoot / targetFile;
		target.deferred = true;
	}

	if (!config.arm7.enabled && !config.arm9.enabled)
		throw ncp::exception("No targets to build were specified.");

	return config;
}

void loadTargets(ProjectConfig& config, const V1Options& options)
{
	if (config.version != 1)
		return;

	// The project variables, restored from their already-expanded values, so a
	// target's $${name} references resolve the same as they would have at
	// project-load time.
	V1Vars projectVars("\"" + config.file.filename().string() + "\"", nullptr, options);
	for (const auto& [name, value] : config.vars)
		projectVars.define(name, value);

	for (bool arm9 : { false, true })
	{
		TargetConfig& target = config.target(arm9);
		if (!target.enabled || !target.deferred)
			continue;

		if (!fs::exists(target.file))
		{
			std::ostringstream oss;
			oss << "The " << OSTR(target.name) << " target file was not found: "
			    << OSTR(target.file.string()) << "."
			    OREASONNL "If a pre-build command generates it, run a build first.";
			throw ncp::exception(oss.str());
		}

		readTargetFile(target, target.file, projectVars);
		target.deferred = false;
	}
}

} // namespace ncp::config
