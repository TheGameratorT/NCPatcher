#pragma once

// The ambient state of a build, passed explicitly.
//
// All of this used to be file-scope statics: BuildConfig's settings, and
// Application's verbose tags and command-line defines. That made every one of
// them readable from anywhere, which was convenient right up until it wasn't --
// nothing could be built twice in one process with different settings, nothing
// could be tested without a global fixture, and `modules dump` could not run in
// a project whose toolchain is not installed because loading the config
// insisted on validating one.
//
// The contents are the same. What changed is that a phase now has to be handed
// them, which is also what makes a per-target view possible: the path anchors
// differ between arm7 and arm9, so each target gets its own copy of a Context
// whose config, options and rebuild record still point at the one set shared by
// the whole run.

#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

#include "../system/path_context.hpp"

namespace ncp {

namespace config {
struct ProjectConfig;
class RebuildStore;
}

// Verbose output categories
enum class VerboseTag
{
	Build,      // Build process and compilation output
	Section,    // Section usage analysis and details
	Elf,        // ELF file analysis and processing
	Patch,      // Patch information and analysis
	Library,    // Library dependency analysis
	Linking,    // Linker script generation and linking process
	Symbols,    // Symbol resolution and analysis
	NoLib,      // Do not print lib patches
	All         // All verbose output (equivalent to old --verbose)
};

// What the invocation asked for, as opposed to what the project says.
struct Options
{
	std::vector<std::string> defines;
	std::unordered_set<VerboseTag> verboseTags;

	[[nodiscard]] bool isVerbose(VerboseTag tag) const;
};

struct Context
{
	// Copied per target, since targetWorkDir and buildDir differ between them.
	PathContext paths;

	// Shared by the whole run; the per-target copies point at the same objects.
	const config::ProjectConfig* config = nullptr;
	const Options* options = nullptr;
	config::RebuildStore* rebuild = nullptr;

	[[nodiscard]] const std::string& toolchain() const;
	[[nodiscard]] int threadCount() const;

	// Absolute, resolved against the project directory.
	[[nodiscard]] std::filesystem::path backupDir() const;

	[[nodiscard]] bool isVerbose(VerboseTag tag) const { return options->isVerbose(tag); }
	[[nodiscard]] const std::vector<std::string>& defines() const { return options->defines; }
};

} // namespace ncp
