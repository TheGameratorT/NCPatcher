#include "migrate.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "config_loader.hpp"
#include "target_resolver.hpp"
#include "../system/log.hpp"
#include "../system/except.hpp"
#include "../utils/util.hpp"

namespace fs = std::filesystem;

// The published copy of schema/ncpatcher.schema.json, which is also installed
// next to ncp.h. The URL rather than the local path, because a configuration
// file is committed and shared, and a path into one machine's install prefix
// would mean nothing on anybody else's.
constexpr std::string_view SCHEMA_URL =
	"https://raw.githubusercontent.com/TheGameratorT/NCPatcher/main/schema/ncpatcher.schema.json";

namespace ncp::config {

namespace {

using Tokens = std::vector<std::string>;

// Flags whose argument is a separate word. Splitting on whitespace alone would
// turn "-x assembler-with-cpp" into two list entries that mean nothing apart,
// and would let the re-factoring hoist one of them without the other.
constexpr std::string_view SEPARATE_ARGUMENT_FLAGS[] = {
	"-x", "-include", "-imacros", "-isystem", "-idirafter", "-iquote",
	"-T", "-Map", "-Xlinker", "-u", "-z", "-b", "-y"
};

bool takesSeparateArgument(std::string_view token)
{
	for (std::string_view flag : SEPARATE_ARGUMENT_FLAGS)
	{
		if (token == flag)
			return true;
	}
	return false;
}

Tokens tokenize(const std::string& text)
{
	Tokens words;
	std::istringstream stream(text);
	std::string word;
	while (stream >> word)
		words.push_back(word);

	Tokens out;
	for (std::size_t i = 0; i < words.size(); i++)
	{
		if (takesSeparateArgument(words[i]) && i + 1 < words.size())
		{
			out.push_back(words[i] + " " + words[i + 1]);
			i++;
			continue;
		}
		out.push_back(words[i]);
	}
	return out;
}

// Linker flags are already a comma-separated -Wl payload in v1, except where a
// project wrote them space-separated and relied on Linker::ldFlagsToGccFlags to
// insert the commas. Splitting on both gives the same list either way, which is
// what lets the converted config compare equal to the original.
Tokens splitLdFlags(const std::string& text)
{
	Tokens out;
	std::size_t start = 0;

	auto flush = [&](std::size_t end) {
		std::string part = text.substr(start, end - start);
		std::size_t from = 0;
		while (true)
		{
			std::size_t split = std::string::npos;
			for (std::size_t i = from; i + 1 < part.size(); i++)
			{
				if (part[i] == ' ' && part[i + 1] == '-')
				{
					split = i;
					break;
				}
			}
			if (split == std::string::npos)
				break;
			out.push_back(part.substr(from, split - from));
			part = part.substr(split + 1);
			from = 0;
		}
		if (!part.empty())
			out.push_back(part);
	};

	while (true)
	{
		const std::size_t comma = text.find(',', start);
		if (comma == std::string::npos)
		{
			flush(text.size());
			break;
		}
		flush(comma);
		start = comma + 1;
	}

	out.erase(std::remove_if(out.begin(), out.end(),
		[](const std::string& s) { return s.empty(); }), out.end());
	return out;
}

bool contains(const Tokens& list, const std::string& value)
{
	return std::find(list.begin(), list.end(), value) != list.end();
}

// Entries present in every list, in the order the first one gives them.
Tokens intersect(const std::vector<const Tokens*>& lists)
{
	Tokens out;
	if (lists.empty())
		return out;

	for (const std::string& candidate : *lists.front())
	{
		if (contains(out, candidate))
			continue;
		const bool everywhere = std::all_of(lists.begin(), lists.end(),
			[&candidate](const Tokens* list) { return contains(*list, candidate); });
		if (everywhere)
			out.push_back(candidate);
	}

	return out;
}

Tokens subtract(const Tokens& from, const Tokens& what)
{
	Tokens out;
	for (const std::string& entry : from)
	{
		if (!contains(what, entry))
			out.push_back(entry);
	}
	return out;
}

// Pulls -DNAME[=VALUE] out of a flag list. Returns the names; the list is left
// without them.
Tokens extractDefines(Tokens& flags)
{
	Tokens defines;
	Tokens rest;
	for (const std::string& flag : flags)
	{
		if (flag.starts_with("-D") && flag.size() > 2)
			defines.push_back(flag.substr(2));
		else
			rest.push_back(flag);
	}
	flags = std::move(rest);
	return defines;
}

std::string_view defineNameOf(std::string_view define)
{
	const std::size_t equals = define.find('=');
	return equals == std::string_view::npos ? define : define.substr(0, equals);
}

// Merges the three languages' define lists into one.
//
// A name that carries different text depending on the language cannot be
// hoisted: hoisting it would have to pick one of the values, and picking is not
// converting. Those names come back in `conflicting` and stay where they were.
Tokens mergeDefines(const Tokens& c, const Tokens& cpp, const Tokens& asm_, Tokens& conflicting)
{
	Tokens order;
	std::vector<std::pair<std::string, std::string>> seen;

	for (const Tokens* list : { &c, &cpp, &asm_ })
	{
		for (const std::string& define : *list)
		{
			const std::string name(defineNameOf(define));
			const auto it = std::find_if(seen.begin(), seen.end(),
				[&name](const auto& entry) { return entry.first == name; });

			if (it == seen.end())
			{
				seen.emplace_back(name, define);
				order.push_back(define);
				continue;
			}
			if (it->second != define && !contains(conflicting, name))
				conflicting.push_back(name);
		}
	}

	Tokens out;
	for (const std::string& define : order)
	{
		if (!contains(conflicting, std::string(defineNameOf(define))))
			out.push_back(define);
	}
	return out;
}

Tokens toDefineFlags(const Tokens& defines)
{
	Tokens out;
	out.reserve(defines.size());
	for (const std::string& define : defines)
		out.push_back("-D" + define);
	return out;
}

// YAML emission ----------------------------------------------------------

bool needsQuoting(const std::string& value)
{
	if (value.empty())
		return true;
	if (value.front() == ' ' || value.back() == ' ')
		return true;
	if (std::string_view("-?:,[]{}#&*!|>'\"%@`").find(value.front()) != std::string_view::npos)
	{
		// A leading '-' only opens a sequence entry when a space follows it, and
		// every compiler flag starts with one, so quoting them all would make
		// the output unreadable for no gain.
		if (value.front() != '-' || (value.size() > 1 && value[1] == ' '))
			return true;
	}
	if (value.find(": ") != std::string::npos || value.find(" #") != std::string::npos)
		return true;
	return false;
}

std::string scalar(const std::string& value)
{
	if (!needsQuoting(value))
		return value;

	std::string out = "\"";
	for (char c : value)
	{
		if (c == '"' || c == '\\')
			out += '\\';
		out += c;
	}
	out += '"';
	return out;
}

// Addresses are padded to eight digits because that is how they are written
// everywhere else -- 0x02026CE0, not 0x2026CE0 -- and a column of ragged
// addresses is harder to scan. Sizes are not, because 0x00056400 reads worse
// than 0x56400.
std::string hex(u32 value, int minDigits = 0)
{
	std::ostringstream oss;
	oss << std::uppercase << std::hex << value;
	std::string digits = oss.str();
	if (int(digits.size()) < minDigits)
		digits.insert(0, std::size_t(minDigits - int(digits.size())), '0');
	return "0x" + digits;
}

constexpr int ADDRESS_DIGITS = 8;

class Emitter
{
public:
	void line(int indent, std::string_view text)
	{
		m_out << std::string(std::size_t(indent) * 2, ' ') << text << '\n';
	}

	void blank() { m_out << '\n'; }

	void comment(int indent, std::string_view text)
	{
		m_out << std::string(std::size_t(indent) * 2, ' ') << "# " << text << '\n';
	}

	void key(int indent, std::string_view name, const std::string& value)
	{
		line(indent, std::string(name) + ": " + scalar(value));
	}

	// Flow style while it stays readable on one line, block style beyond that.
	void list(int indent, std::string_view name, const Tokens& values)
	{
		if (values.empty())
			return;

		std::string flow = std::string(name) + ": [";
		bool flowable = true;
		for (std::size_t i = 0; i < values.size(); i++)
		{
			if (values[i].find_first_of(",[]{}#") != std::string::npos)
			{
				flowable = false;
				break;
			}
			flow += (i ? ", " : "") + scalar(values[i]);
		}
		flow += ']';

		if (flowable && int(flow.size()) + indent * 2 <= 96)
		{
			line(indent, flow);
			return;
		}

		line(indent, std::string(name) + ":");
		for (const std::string& value : values)
			line(indent + 1, "- " + scalar(value));
	}

	[[nodiscard]] std::string str() const { return m_out.str(); }

private:
	std::ostringstream m_out;
};

// The plan ---------------------------------------------------------------

struct LanguageFlags
{
	Tokens common;
	Tokens c;
	Tokens cpp;
	Tokens asm_;
	Tokens ld;
};

struct RegionPlan
{
	const RegionConfig* config = nullptr;
	bool overridesFlags = false;
	LanguageFlags flags;
	Tokens defines;
	bool overridesDefines = false;
};

struct TargetPlan
{
	const TargetConfig* config = nullptr;
	bool enabled = false;
	LanguageFlags flags;
	Tokens defines;
	fs::path workDir;
	std::vector<RegionPlan> regions;
};

Tokens flagTokens(const ListOp& op)
{
	std::string joined;
	for (const std::string& part : op.applyTo({}))
	{
		if (!joined.empty())
			joined += ' ';
		joined += part;
	}
	return tokenize(joined);
}

// Splits a target's three language lists into what they share and what is
// specific to each.
void splitCommon(LanguageFlags& flags)
{
	flags.common = intersect({ &flags.c, &flags.cpp, &flags.asm_ });
	flags.c = subtract(flags.c, flags.common);
	flags.cpp = subtract(flags.cpp, flags.common);
	flags.asm_ = subtract(flags.asm_, flags.common);
}

void buildTargetPlan(TargetPlan& plan, const TargetConfig& target, const fs::path& projectRoot)
{
	plan.config = &target;
	plan.enabled = target.enabled;
	if (!plan.enabled)
		return;

	// v1 has no notion of a target directory: relative paths in a target file
	// resolve against wherever that file happens to sit. v2 has one file, so
	// wherever that was has to be written down.
	const fs::path targetDir = target.workDir.configured()
		? (target.workDir.value.is_absolute() ? target.workDir.value : projectRoot / target.workDir.value)
		: target.file.parent_path();
	plan.workDir = Util::relativeIfSubpath(targetDir, projectRoot);

	plan.flags.c = flagTokens(target.flags.c);
	plan.flags.cpp = flagTokens(target.flags.cpp);
	plan.flags.asm_ = flagTokens(target.flags.asm_);

	Tokens definesC = extractDefines(plan.flags.c);
	Tokens definesCpp = extractDefines(plan.flags.cpp);
	Tokens definesAsm = extractDefines(plan.flags.asm_);

	// In v2 a define reaches all three languages -- assembly included, since it
	// is compiled through -x assembler-with-cpp. So the union is hoisted, not
	// the intersection, and the assembler gains the defines v1 only gave to C.
	// That is the one thing the conversion deliberately changes, and the
	// self-check below is what proves it changed nothing else.
	Tokens conflicting;
	plan.defines = mergeDefines(definesC, definesCpp, definesAsm, conflicting);

	auto reattach = [&](Tokens& flags, const Tokens& defines) {
		const Tokens leftover = subtract(defines, plan.defines);
		const Tokens asFlags = toDefineFlags(leftover);
		flags.insert(flags.end(), asFlags.begin(), asFlags.end());
	};
	reattach(plan.flags.c, definesC);
	reattach(plan.flags.cpp, definesCpp);
	reattach(plan.flags.asm_, definesAsm);

	splitCommon(plan.flags);

	std::string ld;
	for (const std::string& part : target.flags.ld.applyTo({}))
	{
		if (!ld.empty())
			ld += ',';
		ld += part;
	}
	plan.flags.ld = splitLdFlags(ld);

	for (const RegionConfig& region : target.regions)
	{
		RegionPlan regionPlan;
		regionPlan.config = &region;
		regionPlan.overridesFlags = !region.flags.c.empty() || !region.flags.cpp.empty()
			|| !region.flags.asm_.empty();

		if (regionPlan.overridesFlags)
		{
			// A v1 region flag string replaced the target's outright, and it
			// replaced all of it -- so the converted region has to say so for
			// every language, not just the one it happened to mention.
			auto pick = [&](const ListOp& regionOp, const Tokens& fallback) {
				return regionOp.empty() ? fallback : flagTokens(regionOp);
			};
			Tokens c = pick(region.flags.c, {});
			Tokens cpp = pick(region.flags.cpp, {});
			Tokens asm_ = pick(region.flags.asm_, {});

			// Where the region said nothing, it inherited the target's whole
			// string, which by now has been split up; put it back together.
			auto rebuild = [&](const Tokens& specific) {
				Tokens all = plan.flags.common;
				all.insert(all.end(), specific.begin(), specific.end());
				return all;
			};
			if (region.flags.c.empty()) c = rebuild(plan.flags.c);
			if (region.flags.cpp.empty()) cpp = rebuild(plan.flags.cpp);
			if (region.flags.asm_.empty()) asm_ = rebuild(plan.flags.asm_);

			Tokens dc = extractDefines(c);
			Tokens dcpp = extractDefines(cpp);
			Tokens dasm = extractDefines(asm_);
			Tokens regionConflicting;
			regionPlan.defines = mergeDefines(dc, dcpp, dasm, regionConflicting);
			regionPlan.overridesDefines = regionPlan.defines != plan.defines;

			auto reattachRegion = [&](Tokens& flags, const Tokens& defines) {
				const Tokens leftover = subtract(defines, regionPlan.defines);
				const Tokens asFlags = toDefineFlags(leftover);
				flags.insert(flags.end(), asFlags.begin(), asFlags.end());
			};
			reattachRegion(c, dc);
			reattachRegion(cpp, dcpp);
			reattachRegion(asm_, dasm);

			regionPlan.flags.c = std::move(c);
			regionPlan.flags.cpp = std::move(cpp);
			regionPlan.flags.asm_ = std::move(asm_);
		}

		plan.regions.push_back(std::move(regionPlan));
	}
}

// Hoists everything both targets agree on to the project level.
void hoistToProject(LanguageFlags& project, Tokens& projectDefines,
                    std::vector<TargetPlan*>& targets)
{
	if (targets.empty())
		return;

	auto hoist = [&targets](Tokens LanguageFlags::* member, Tokens& into) {
		std::vector<const Tokens*> lists;
		for (const TargetPlan* target : targets)
			lists.push_back(&(target->flags.*member));
		into = intersect(lists);
		for (TargetPlan* target : targets)
			target->flags.*member = subtract(target->flags.*member, into);
	};

	hoist(&LanguageFlags::common, project.common);
	hoist(&LanguageFlags::c, project.c);
	hoist(&LanguageFlags::cpp, project.cpp);
	hoist(&LanguageFlags::asm_, project.asm_);
	hoist(&LanguageFlags::ld, project.ld);

	std::vector<const Tokens*> defineLists;
	for (const TargetPlan* target : targets)
		defineLists.push_back(&target->defines);
	projectDefines = intersect(defineLists);
	for (TargetPlan* target : targets)
		target->defines = subtract(target->defines, projectDefines);
}

void emitFlags(Emitter& emitter, int indent, const LanguageFlags& flags)
{
	if (flags.common.empty() && flags.c.empty() && flags.cpp.empty()
		&& flags.asm_.empty() && flags.ld.empty())
		return;

	emitter.line(indent, "flags:");
	emitter.list(indent + 1, "common", flags.common);
	emitter.list(indent + 1, "c", flags.c);
	emitter.list(indent + 1, "cpp", flags.cpp);
	emitter.list(indent + 1, "asm", flags.asm_);
	emitter.list(indent + 1, "ld", flags.ld);
}

void emitRegion(Emitter& emitter, int indent, const RegionPlan& plan)
{
	const RegionConfig& region = *plan.config;

	emitter.line(indent, "- dest: " + scalar(region.dest));
	const int body = indent + 1;

	if (region.mode != RegionMode::Append)
		emitter.key(body, "mode", regionModeName(region.mode));
	if (region.address.configured())
		emitter.key(body, "address", hex(region.address.value, ADDRESS_DIGITS));
	if (region.maxsize.configured())
		emitter.key(body, "maxsize", hex(region.maxsize.value));
	if (region.compress)
		emitter.key(body, "compress", "true");

	emitter.list(body, "sources", region.sources.applyTo({}));

	if (plan.overridesDefines)
	{
		emitter.line(body, "defines:");
		emitter.list(body + 1, "set", plan.defines);
	}

	if (plan.overridesFlags)
	{
		// `set:` on every list, including an empty one for common: a v1 region
		// flag string replaced everything it inherited, and append -- the v2
		// default -- would silently keep it.
		emitter.line(body, "flags:");
		emitter.line(body + 1, "common: { set: [] }");
		emitter.line(body + 1, "c:");
		emitter.list(body + 2, "set", plan.flags.c);
		emitter.line(body + 1, "cpp:");
		emitter.list(body + 2, "set", plan.flags.cpp);
		emitter.line(body + 1, "asm:");
		emitter.list(body + 2, "set", plan.flags.asm_);
	}

	if (!region.overwrites.empty())
	{
		emitter.line(body, "overwrites:");
		for (const Overwrite& overwrite : region.overwrites)
		{
			emitter.line(body + 1, "- [" + hex(overwrite.startAddress, ADDRESS_DIGITS) + ", "
				+ hex(overwrite.endAddress, ADDRESS_DIGITS) + "]");
		}
	}
}

std::string emitDocument(const ProjectConfig& config, const LanguageFlags& projectFlags,
                         const Tokens& projectDefines, const std::vector<TargetPlan>& targets,
                         const fs::path& projectFile)
{
	Emitter emitter;

	// Read by the YAML Language Server, which every editor with YAML support
	// speaks: completion and inline validation for a schema nobody has learned
	// yet is most of what makes migrating cheap. It is a comment, so any other
	// tool ignores it.
	emitter.comment(0, "yaml-language-server: $schema=" + std::string(SCHEMA_URL));

	emitter.comment(0, "Converted from " + projectFile.filename().string()
		+ " by `ncpatcher migrate`.");
	emitter.comment(0, "");
	emitter.comment(0, "Flags shared by both targets, and by a target's three languages, have been");
	emitter.comment(0, "hoisted; every -D has become an entry in a defines list. Lists inherit by");
	emitter.comment(0, "appending, so a target adds to the project's flags rather than restating");
	emitter.comment(0, "them. To subtract instead, write { remove: [...] } or { set: [...] }.");
	emitter.line(0, "version: 2");
	emitter.blank();

	emitter.line(0, "rom:");
	emitter.key(1, "dir", config.filesystemDir.value.generic_string());
	emitter.key(1, "backup", config.backupDir.value.generic_string());
	emitter.blank();

	emitter.line(0, "toolchain:");
	emitter.key(1, "prefix", config.toolchain.value);
	emitter.blank();

	if (config.threadCount.value != 0)
	{
		emitter.line(0, "build:");
		emitter.key(1, "threads", std::to_string(config.threadCount.value));
		emitter.blank();
	}

	if (!projectDefines.empty())
	{
		emitter.list(0, "defines", projectDefines);
		emitter.blank();
	}

	if (!projectFlags.common.empty() || !projectFlags.c.empty() || !projectFlags.cpp.empty()
		|| !projectFlags.asm_.empty() || !projectFlags.ld.empty())
	{
		emitFlags(emitter, 0, projectFlags);
		emitter.blank();
	}

	if (!config.preBuild.empty())
	{
		emitter.list(0, "pre-build", config.preBuild);
		emitter.blank();
	}
	if (!config.postBuild.empty())
	{
		emitter.list(0, "post-build", config.postBuild);
		emitter.blank();
	}

	emitter.line(0, "targets:");
	for (const TargetPlan& plan : targets)
	{
		const TargetConfig& target = *plan.config;
		if (!plan.enabled)
		{
			emitter.line(1, target.name + ": { enabled: false }");
			continue;
		}

		emitter.line(1, target.name + ":");
		emitter.key(2, "build", target.buildDir.value.generic_string());
		if (!plan.workDir.empty() && plan.workDir != ".")
			emitter.key(2, "workdir", plan.workDir.generic_string());
		if (target.arenaLo.configured())
			emitter.key(2, "arena-lo", hex(target.arenaLo.value, ADDRESS_DIGITS));
		if (target.symbols.configured())
			emitter.key(2, "symbols", target.symbols.value.generic_string());

		emitter.list(2, "includes", target.includes.applyTo({}));
		emitter.list(2, "defines", plan.defines);
		emitFlags(emitter, 2, plan.flags);

		emitter.line(2, "regions:");
		for (const RegionPlan& region : plan.regions)
			emitRegion(emitter, 3, region);
	}

	return emitter.str();
}

// Verification -----------------------------------------------------------

Tokens sortedTokens(const std::string& flags)
{
	Tokens out = tokenize(flags);
	std::sort(out.begin(), out.end());
	return out;
}

std::vector<std::string> sortedPaths(const std::vector<fs::path>& paths)
{
	std::vector<std::string> out;
	out.reserve(paths.size());
	for (const fs::path& path : paths)
		out.push_back(path.generic_string());
	std::sort(out.begin(), out.end());
	return out;
}

class Comparison
{
public:
	void check(bool equal, const std::string& what)
	{
		if (!equal)
			m_differences.push_back(what);
	}

	template <typename T>
	void checkEqual(const T& before, const T& after, const std::string& what)
	{
		check(before == after, what);
	}

	// Flag lists are the one place the conversion is allowed to differ, and
	// only in one direction: v2 gives every define to all three languages, so
	// the assembler may gain -D switches it did not have. Anything else added,
	// and anything at all removed, is a bug in the conversion.
	void checkFlags(const std::string& before, const std::string& after, const std::string& what)
	{
		const Tokens beforeTokens = sortedTokens(before);
		const Tokens afterTokens = sortedTokens(after);

		for (const std::string& token : subtract(beforeTokens, afterTokens))
			m_differences.push_back(what + ": lost " + token);

		for (const std::string& token : subtract(afterTokens, beforeTokens))
		{
			if (token.starts_with("-D"))
			{
				if (!contains(m_addedDefines, token))
					m_addedDefines.push_back(token);
				continue;
			}
			m_differences.push_back(what + ": gained " + token);
		}
	}

	[[nodiscard]] const std::vector<std::string>& differences() const { return m_differences; }
	[[nodiscard]] const Tokens& addedDefines() const { return m_addedDefines; }

private:
	std::vector<std::string> m_differences;
	Tokens m_addedDefines;
};

void compareTargets(Comparison& comparison, const BuildTarget& before, const BuildTarget& after,
                    const std::string& name)
{
	comparison.checkEqual(before.arenaLo, after.arenaLo, name + ": arenaLo");
	comparison.checkEqual(before.symbols.generic_string(), after.symbols.generic_string(),
		name + ": symbols");
	comparison.checkEqual(sortedPaths(before.includes), sortedPaths(after.includes),
		name + ": includes");

	{
		Tokens beforeLd = splitLdFlags(before.ldFlags);
		Tokens afterLd = splitLdFlags(after.ldFlags);
		std::sort(beforeLd.begin(), beforeLd.end());
		std::sort(afterLd.begin(), afterLd.end());
		comparison.checkEqual(beforeLd, afterLd, name + ": ld flags");
	}

	if (before.regions.size() != after.regions.size())
	{
		comparison.check(false, name + ": region count");
		return;
	}

	for (std::size_t i = 0; i < before.regions.size(); i++)
	{
		const BuildTarget::Region& a = before.regions[i];
		const BuildTarget::Region& b = after.regions[i];
		const std::string where = name + ": region " + std::to_string(a.destination);

		comparison.checkEqual(a.destination, b.destination, where + " destination");
		comparison.checkEqual(int(a.mode), int(b.mode), where + " mode");
		comparison.checkEqual(a.address, b.address, where + " address");
		comparison.checkEqual(a.maxsize, b.maxsize, where + " maxsize");
		comparison.checkEqual(a.compress, b.compress, where + " compress");
		comparison.checkEqual(sortedPaths(a.sources), sortedPaths(b.sources), where + " sources");
		comparison.checkFlags(a.cFlags, b.cFlags, where + " c flags");
		comparison.checkFlags(a.cppFlags, b.cppFlags, where + " cpp flags");
		comparison.checkFlags(a.asmFlags, b.asmFlags, where + " asm flags");
		comparison.checkEqual(a.overwrites.size(), b.overwrites.size(), where + " overwrite count");

		for (std::size_t j = 0; j < a.overwrites.size() && j < b.overwrites.size(); j++)
		{
			comparison.check(a.overwrites[j].startAddress == b.overwrites[j].startAddress
				&& a.overwrites[j].endAddress == b.overwrites[j].endAddress,
				where + " overwrite " + std::to_string(j));
		}
	}
}

PathContext targetPaths(const ProjectConfig& config, const TargetConfig& target)
{
	PathContext paths;
	paths.workDir = config.projectRoot;
	paths.targetWorkDir = target.workDir.configured()
		? paths.work(target.workDir.value)
		: target.file.parent_path();
	return paths;
}

} // namespace

bool migrate(const fs::path& projectFile, const fs::path& projectRoot, bool write)
{
	Log::info("Reading " + projectFile.filename().string() + "...");

	ProjectConfig original = loadV1(projectFile, projectRoot);

	if (original.version != 1)
	{
		Log::out << OINFO << OSTR(projectFile.filename().string())
		         << " is already in the version 2 schema; nothing to do." << std::endl;
		return true;
	}

	loadTargets(original);

	// A second read that leaves ${env:X} standing, so the emitted config still
	// asks the environment instead of hard-coding one machine's answer.
	V1Options portableOptions;
	portableOptions.keepEnvironmentReferences = true;
	portableOptions.quiet = true;
	ProjectConfig portable = loadV1(projectFile, projectRoot, portableOptions);
	loadTargets(portable, portableOptions);

	std::vector<TargetPlan> plans(2);
	buildTargetPlan(plans[0], portable.arm7, projectRoot);
	buildTargetPlan(plans[1], portable.arm9, projectRoot);

	std::vector<TargetPlan*> enabled;
	for (TargetPlan& plan : plans)
	{
		if (plan.enabled)
			enabled.push_back(&plan);
	}

	LanguageFlags projectFlags;
	Tokens projectDefines;
	hoistToProject(projectFlags, projectDefines, enabled);

	const std::string document = emitDocument(portable, projectFlags, projectDefines,
		plans, projectFile);

	// Self-check: read the converted document back and resolve both schemas.
	// Hoisting reorders flags, and the only trustworthy way to say that did not
	// change anything is to compare the two resolved configurations.
	const fs::path outputFile = projectRoot / "ncpatcher.yaml";
	ProjectConfig converted;
	{
		const fs::path scratch = projectRoot / ".ncpatcher-migrate.tmp.yaml";
		{
			std::ofstream out(scratch, std::ios::binary);
			if (!out.is_open())
				throw ncp::file_error(scratch, ncp::file_error::write);
			out << document;
		}

		std::error_code ignored;
		try {
			converted = loadV2(scratch, projectRoot);
		} catch (...) {
			fs::remove(scratch, ignored);
			throw;
		}
		fs::remove(scratch, ignored);
	}
	// The converted targets say where they live; the temporary file they were
	// read from does not, so point them at where the real one will be.
	converted.arm7.file = outputFile;
	converted.arm9.file = outputFile;

	Comparison comparison;
	TargetResolver::Options quiet;
	quiet.quiet = true;

	comparison.checkEqual(original.backupDir.value.generic_string(),
		converted.backupDir.value.generic_string(), "backup directory");
	comparison.checkEqual(original.filesystemDir.value.generic_string(),
		converted.filesystemDir.value.generic_string(), "filesystem directory");
	comparison.checkEqual(original.toolchain.value, converted.toolchain.value, "toolchain");
	comparison.checkEqual(original.preBuild, converted.preBuild, "pre-build commands");
	comparison.checkEqual(original.postBuild, converted.postBuild, "post-build commands");

	for (bool arm9 : { false, true })
	{
		const TargetConfig& before = original.target(arm9);
		const TargetConfig& after = converted.target(arm9);
		const std::string name = arm9 ? "arm9" : "arm7";

		if (before.enabled != after.enabled)
		{
			comparison.check(false, name + ": enabled");
			continue;
		}
		if (!before.enabled)
			continue;

		const BuildTarget resolvedBefore =
			TargetResolver::resolve(original, before, targetPaths(original, before), quiet);
		const BuildTarget resolvedAfter =
			TargetResolver::resolve(converted, after, targetPaths(converted, after), quiet);
		compareTargets(comparison, resolvedBefore, resolvedAfter, name);
	}

	if (!comparison.differences().empty())
	{
		std::ostringstream oss;
		oss << "The converted configuration does not resolve to the same build.";
		for (const std::string& difference : comparison.differences())
			oss << OREASONNL "differs: " << difference;
		oss << OREASONNL "Nothing was written. Please report this with the configuration attached.";
		throw ncp::exception(oss.str());
	}

	if (comparison.addedDefines().empty())
	{
		Log::info("Converted configuration resolves identically to the original.");
	}
	else
	{
		std::ostringstream oss;
		oss << "Converted configuration resolves identically, with one intended change."
		    OREASONNL "These defines now reach the assembler as well as C and C++:";
		for (const std::string& define : comparison.addedDefines())
			oss << OREASONNL "  " << define;
		Log::out << OINFO << oss.str() << std::endl;
	}

	if (!write)
	{
		Log::out << std::endl << document << std::endl;
		Log::out << OINFO << "Nothing written. Re-run with " ANSI_bCYAN "--write" ANSI_RESET
		         << " to save this as " << OSTRa(outputFile.filename().string()) << "." << std::endl;
		return true;
	}

	if (fs::exists(outputFile))
	{
		std::ostringstream oss;
		oss << OSTR(outputFile.filename().string()) << " already exists."
		    OREASONNL "Move it aside before converting, so nothing is overwritten.";
		throw ncp::exception(oss.str());
	}

	std::ofstream out(outputFile, std::ios::binary);
	if (!out.is_open())
		throw ncp::file_error(outputFile, ncp::file_error::write);
	out << document;
	out.close();

	Log::out << OINFO << "Wrote " << OSTR(outputFile.filename().string()) << "."
	         OREASONNL << OSTRa(projectFile.filename().string())
	         << " is still read if the new file is removed; delete it once the build is verified."
	         << std::endl;

	return true;
}

} // namespace ncp::config
