#pragma once

// The parsed shape of a project's configuration, before any of it is resolved.
//
// This is deliberately not BuildTarget. What the file says and what the build
// needs are different things: the file says "append -O2 to the inherited cpp
// flags for this one region", the build needs one finished command-line string.
// Keeping the two apart is what lets three levels of inheritance, a v1 reader
// and a v2 reader, and a migration tool all meet in one place -- and what lets
// `config dump` answer "why is this flag here" at all.
//
// TargetResolver turns this into the BuildTarget that source/build and
// source/patch consume, which is unchanged.

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "node.hpp"
#include "../utils/types.hpp"

namespace ncp::config {

// Where a setting's value came from. Tracked so a build can explain itself
// rather than leaving the user to guess which of four places won.
enum class Source
{
	Default,
	ProjectFile,
	TargetSection,
	RegionSection,
	Environment,
	CommandLine
};

[[nodiscard]] const char* sourceName(Source source);

template <typename T>
struct Setting
{
	T value{};
	Source source = Source::Default;

	void set(T newValue, Source newSource)
	{
		value = std::move(newValue);
		source = newSource;
	}

	// True once something other than the built-in default has spoken.
	[[nodiscard]] bool configured() const { return source != Source::Default; }
};

// One level's contribution to a list-valued setting.
//
// Every inherited list defaults to append, because that is what the projects
// this replaces were emulating by hand with $${var} string concatenation. The
// mapping form exists for the cases append cannot express -- a region that
// needs -O2 where the project said -Os -- and is spelled out rather than
// encoded in sigils, so a reader who has never seen the syntax can still guess
// what it does.
struct ListOp
{
	// `set:` replaces whatever was inherited. The flag distinguishes an explicit
	// `set: []`, which means "nothing", from no `set:` at all.
	bool hasSet = false;
	std::vector<std::string> set;
	std::vector<std::string> remove;
	std::vector<std::string> append;

	[[nodiscard]] bool empty() const;

	// set, then remove, then append -- so a level can replace an inherited entry
	// in one step without caring whether it was there.
	[[nodiscard]] std::vector<std::string> applyTo(std::vector<std::string> inherited) const;
};

// Preprocessor defines, insertion-ordered and keyed by name.
//
// Ordered because the order reaches the compiler command line. Keyed because
// the same name arriving twice with different values is worth reporting: taking
// the last one silently is how a module ends up compiled against a different
// constant than the code calling it.
class DefineSet
{
public:
	struct Define
	{
		std::string name;
		std::string value;
		bool hasValue = false;
		// Human-readable "where this came from", used in the conflict warning.
		std::string origin;
	};

	// Accepts "NAME" and "NAME=VALUE".
	//
	// `warnOnConflict` is the caller's call because the same operation means
	// different things at different levels: a target overriding a project
	// define is ordinary inheritance, while two modules defining the same name
	// differently is a collision nobody asked for.
	void add(std::string_view text, std::string origin, bool warnOnConflict = false);
	void add(Define define, bool warnOnConflict = false);
	void merge(const DefineSet& other, bool warnOnConflict = false);
	void remove(std::string_view name);

	[[nodiscard]] bool empty() const { return m_defines.empty(); }
	[[nodiscard]] const std::vector<Define>& entries() const { return m_defines; }
	[[nodiscard]] const Define* find(std::string_view name) const;

	// "-DNAME=VALUE", ready to join into a command line.
	[[nodiscard]] std::vector<std::string> toFlags() const;

	// "NAME=VALUE", the form the rebuild hash and --define share.
	[[nodiscard]] std::vector<std::string> toStrings() const;

private:
	std::vector<Define> m_defines;
};

// The five flag lists, as one level of the chain contributes them.
// asm_ is spelled with the underscore because `asm` is a C++ keyword.
struct FlagOps
{
	ListOp common;
	ListOp c;
	ListOp cpp;
	ListOp asm_;
	ListOp ld;

	[[nodiscard]] bool empty() const;
};

// The same five, resolved.
struct FlagLists
{
	std::vector<std::string> common;
	std::vector<std::string> c;
	std::vector<std::string> cpp;
	std::vector<std::string> asm_;
	std::vector<std::string> ld;

	[[nodiscard]] FlagLists inheritedBy(const FlagOps& ops) const;
};

enum class RegionMode
{
	Append = 0,
	Replace,
	Create
};

[[nodiscard]] const char* regionModeName(RegionMode mode);

struct Overwrite
{
	u32 startAddress = 0;
	u32 endAddress = 0;
};

struct RegionConfig
{
	// As written -- "main" or "ovNN" -- kept for diagnostics that should echo
	// the user's own spelling.
	std::string dest;

	// Parsed: -1 for main, otherwise the overlay id.
	int destination = -1;

	RegionMode mode = RegionMode::Append;
	bool compress = false;

	Setting<u32> address;
	Setting<u32> maxsize;

	std::vector<Overwrite> overwrites;

	// Glob patterns, not yet expanded: expansion needs a base directory that
	// only the resolver knows, and modules must be folded in before it happens.
	ListOp sources;

	FlagOps flags;

	// Kept as an operation rather than a resolved set for the same reason the
	// flag lists are: which level contributed a define is part of the answer to
	// "why is this defined", and merging early throws that away.
	ListOp defines;

	cfg::Mark mark;
};

// What the project says about one component of one module.
//
// Everything here is optional: a project that only wants a module switched on
// writes the module's name and nothing else. The flags distinguish "said false"
// from "said nothing", which matters because saying nothing means the module's
// own answer stands.
struct ComponentOverride
{
	std::string name;

	bool hasEnabled = false;
	bool enabled = true;

	// Empty when the project did not retarget the component.
	std::string target;

	// Values for defines the component already declares, by name. Adding a
	// define the component never had is not an override and is rejected: it
	// would land in the build with nothing to explain where it came from.
	std::vector<std::pair<std::string, std::string>> defines;

	// cfg::Node::location(), kept rather than the node -- the project document
	// is closed long before modules are resolved.
	std::string location;
};

// One entry of `modules.enabled`.
struct ModuleSelection
{
	std::string key;
	bool enabled = true;

	// A module that may simply not be there. Without this, a missing directory
	// is an error: the alternative is a project that quietly builds without a
	// feature it asked for, which is how the prototype behaved and how a
	// mistyped module name became a mystery.
	bool optional = false;

	std::vector<ComponentOverride> components;

	std::string location;
};

struct ModulesConfig
{
	// True once the project has a `modules:` section at all, whatever is in it.
	// An absent section and one that enables nothing are different: only the
	// first leaves declared-but-empty regions alone.
	bool present = false;

	Setting<std::filesystem::path> dir;

	// Where to write the machine-readable graph. Written before the pre-build
	// commands run, so a generator that consumes it is an ordinary hook.
	Setting<std::filesystem::path> dump;

	// Lets a component target an overlay the target never declared a region
	// for. Off by default: a typo in an overlay id would otherwise produce a
	// silently empty overlay instead of an error.
	bool autoCreateRegions = false;

	std::vector<ModuleSelection> selections;

	[[nodiscard]] bool empty() const { return selections.empty(); }
};

struct TargetConfig
{
	std::string name;          // "arm9" or "arm7"
	bool arm9 = false;
	bool enabled = false;

	// The file this target's settings were read from. In v1 that is a separate
	// JSON; in v2 it is the project file itself.
	std::filesystem::path file;

	// True while a v1 target's own file has not been read yet.
	//
	// It cannot be read when the project file is: a project may generate its
	// target JSON from a pre-build command, and those run after the project
	// configuration is loaded. nsmb-coop-module does exactly that. So v1
	// targets are opened by loadTargets(), which the build calls once the
	// pre-build commands have had their chance to write them.
	bool deferred = false;

	Setting<std::filesystem::path> buildDir;
	Setting<std::filesystem::path> workDir;
	Setting<std::filesystem::path> symbols;
	Setting<u32> arenaLo;

	ListOp includes;
	FlagOps flags;
	ListOp defines;
	std::vector<RegionConfig> regions;

	cfg::Mark mark;
};

struct ProjectConfig
{
	// 1 for the JSON schema, 2 for YAML. Read from `version:`; its absence is
	// what identifies a v1 document.
	int version = 1;

	std::filesystem::path file;
	std::filesystem::path projectRoot;

	Setting<std::filesystem::path> backupDir;

	// Extracted-ROM directory. Mutually exclusive with romFile.
	Setting<std::filesystem::path> filesystemDir;

	// A .nds read directly, as opposed to an extracted directory.
	Setting<std::filesystem::path> romFile;

	// Where the patched .nds goes. Empty means patch rom.file in place. Only
	// meaningful alongside romFile: an extracted directory is patched where it
	// is, because there is nothing to copy it into.
	Setting<std::filesystem::path> romOutput;

	// Which extracted layout the ROM directory uses -- see rom/dir_accessor.hpp
	// for the presets. Ignored when romFile is set, since a .nds has only one
	// layout.
	Setting<std::string> romLayoutPreset;

	// Per-file overrides on top of the preset, keyed by the config's own names
	// ("arm9-ovt", "overlay9-name", ...). Kept as written so that an unknown
	// key can be reported against the line it came from.
	std::vector<std::pair<std::string, std::string>> romLayoutOverrides;

	// Spare bytes left after arm9 in a .nds, so that an `append` region growing
	// it does not force the whole ROM to be laid out again on every build.
	// Unset means "choose one"; see rom/nds_rom.cpp.
	Setting<u32> romArm9Slack;

	Setting<std::string> toolchain;
	Setting<int> threadCount;

	ListOp includes;
	FlagOps flags;
	ListOp defines;

	ModulesConfig modules;

	std::vector<std::string> preBuild;
	std::vector<std::string> postBuild;

	TargetConfig arm7;
	TargetConfig arm9;

	// Declared `vars:`, already expanded. Kept for `${vars.X}` lookups and for
	// migrate, which re-emits them.
	std::unordered_map<std::string, std::string> vars;

	[[nodiscard]] const TargetConfig& target(bool isArm9) const { return isArm9 ? arm9 : arm7; }
	[[nodiscard]] TargetConfig& target(bool isArm9) { return isArm9 ? arm9 : arm7; }
};

} // namespace ncp::config
