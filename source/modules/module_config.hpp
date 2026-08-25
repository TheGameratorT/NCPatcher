#pragma once

// What a module.yaml says, before anything has been decided about it.
//
// Kept deliberately close to the file: a component that names a target the
// project later overrides still remembers what the module asked for, because
// "why is this component in overlay 9" is a question `modules explain` has to
// answer with both halves of the story.
//
// The vocabulary is the one the Python prototype established, because eight
// modules are already written against it. What is new is that the shapes it
// accepted by accident -- a component name appearing twice, a source file two
// components both claim -- are diagnosed here instead of resolving to whichever
// one happened to be last.

#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../config/node.hpp"

namespace ncp::modules {

// "arm9", "arm7", "arm9(12)". A leading '!' locks the choice, so that a project
// overriding it is told rather than silently ignored.
struct TargetRef
{
	bool valid = false;   // false when the module named no target at all
	bool arm9 = true;
	int overlay = -1;     // -1 is the main binary
	bool locked = false;

	// "arm9", "arm9(12)" -- without the lock marker, which is not part of the
	// identity of the target.
	[[nodiscard]] std::string str() const;

	// "main" / "ov12", the spelling regions use.
	[[nodiscard]] std::string regionName() const;

	[[nodiscard]] bool operator==(const TargetRef& other) const
	{
		return valid == other.valid && arm9 == other.arm9 && overlay == other.overlay;
	}
};

// Parses one target string. Returns false and fills `error` on anything that is
// not a processor optionally followed by an overlay id.
[[nodiscard]] bool parseTargetRef(std::string_view text, TargetRef& out, std::string& error);

// A define as the file wrote it: "NAME" or "NAME=VALUE".
struct DefineText
{
	std::string text;
	cfg::Mark mark;
};

// One entry of a module's `targets:` -- the catch-all that sweeps a directory
// into a region, and the include directories that come with it.
struct ModuleTarget
{
	std::string text;      // as written, lock marker included
	TargetRef target;
	std::vector<std::string> includes;
	std::vector<std::string> sources;
	cfg::Mark mark;
};

// One entry of `components:`.
//
// A component is the unit a project switches on and off. Everything it owns --
// its sources, its defines, the files it wants in the filesystem -- disappears
// with it, which is the whole reason the module system exists.
// A directory the module sweeps into NitroFS, declared once instead of listing
// every file. The shape mirrors the project's `file-trees:` entry -- see
// config/project_config.hpp -- because a module's filesystem and the project's
// are the same kind of thing and resolve through the same code.
struct NitroFsDef
{
	// False when the module said nothing, which is the usual case.
	bool declared = false;

	// Relative to the module directory.
	std::string dir;

	bool layered = false;
	std::string baseVariant;
	std::string into;

	cfg::Mark mark;
};

struct ComponentDef
{
	std::string name;
	cfg::Mark mark;

	// Empty when the component declares no target of its own, which is how a
	// defines-only component is written.
	std::string targetText;
	TargetRef target;

	std::vector<std::string> sources;
	std::vector<std::string> includes;
	std::vector<DefineText> defines;
	std::vector<std::string> files;
	std::vector<std::string> requires_;

	// Keys this program does not know. They are kept verbatim and re-emitted in
	// the dump: `objects:` is meaningless here and load-bearing to the generator
	// that reads the dump, and inventing a schema for it would only mean two
	// programs having to agree on it.
	std::vector<std::pair<std::string, cfg::Node>> extra;
};

// One module.yaml.
struct ModuleDef
{
	// The directory name, which is how the project refers to it.
	std::string key;

	std::string id;
	std::string name;
	std::string description;
	std::string repo;
	std::vector<std::string> authors;

	std::vector<DefineText> defines;
	std::vector<ModuleTarget> targets;
	std::vector<ComponentDef> components;

	NitroFsDef nitrofs;

	// Module-level keys this program does not know, kept for the same reason
	// ComponentDef::extra is: a game-specific generator declares things here
	// that mean nothing to a patcher -- `level-data:` is the motivating case --
	// and inventing a schema for them would only mean two programs having to
	// agree on one.
	std::vector<std::pair<std::string, cfg::Node>> extra;

	std::filesystem::path dir;
	std::filesystem::path file;

	// Held so that the cfg::Node handles above stay valid: a Node points back
	// at the document that produced it.
	std::shared_ptr<cfg::Document> document;

	[[nodiscard]] const ComponentDef* findComponent(std::string_view name) const;
};

// Reads one module.yaml. Throws cfg_error, positioned, on anything malformed.
// `key` is the directory name; it is not read from the file.
[[nodiscard]] ModuleDef loadModuleFile(const std::filesystem::path& file, std::string key);

} // namespace ncp::modules
