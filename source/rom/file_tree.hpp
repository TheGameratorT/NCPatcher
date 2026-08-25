#pragma once

// Directories swept wholesale into NitroFS.
//
// `files:` is a mapping of one ROM path to one source, which is the right shape
// for a project that changes three files and the wrong shape for one that ships
// a filesystem. nsmb-coop-module's nine languages come to 714 entries; writing
// them out is why a Python script existed to generate them.
//
// A tree replaces the list with the directory itself: whatever is under it is
// what the ROM gets, at the same relative path. Two properties make that usable
// for more than one project at a time.
//
// *Layering.* A layered tree reads its first path segment as a variant name
// rather than part of the ROM path, so `nitrofs/fr/ARCHIVE/x.bin` is `x.bin`
// for the French build and nothing at all for the German one. A `base-variant`
// is applied underneath, which is what lets a project translate eight files out
// of two thousand: the base supplies everything the variant does not override.
// A variant may supply a file the base never had -- that is ordinary, not a
// missing base -- and a variant directory that does not exist contributes
// nothing rather than failing.
//
// *Ownership.* Every swept file records the module and component it came from,
// which is what the manifest reports and what an editor needs in order to say
// which module owns a path and which languages translate it. Ownership also
// runs backwards: a component that is switched off subtracts its `files:`
// patterns from its own module's tree, so disabling a feature removes its
// assets as well as its code.
//
// Two rules settle who wins a destination, and they apply in this order.
// Layering first: a file chosen for the built variant outranks one that applies
// to every variant, so a module translating one language beats a module
// supplying the stock version for all of them. Then module order: within one
// layer the first tree to claim a destination keeps it, and `modules.enabled`
// is that order. The second rule is precedence, not a tiebreak -- a module that
// replaces a piece of artwork wholesale has to outrank one that only translates
// what it replaced, and listing it first is how the project says so.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "../config/project_config.hpp"

namespace ncp::rom {

// One directory to sweep, with everything already settled: the source path is
// absolute, and the component patterns are the owning module's.
struct FileTree
{
	std::filesystem::path dir;
	bool layered = false;
	std::string baseVariant;
	std::string into;

	// Empty for the project's own trees.
	std::string module;

	// The layer to read for the built variant, when the project mapped this
	// module to a layer that is not simply the variant's name. Unset means the
	// variant's own name, which is the usual case.
	//
	// Setting it also makes the layer mandatory: a variant directory that is
	// missing is normally fine -- a module translates some languages and not
	// others -- but a project that named a layer explicitly has asserted it is
	// there, and honouring a typo by quietly falling back to the base is how a
	// build ships without its translations.
	std::optional<std::string> variantLayer;

	// What a diagnostic calls this tree.
	std::string origin;

	// The owning module's components, in declaration order. The first pattern
	// that matches a destination decides that file's component provenance, and
	// when that component is disabled, that the file is not inserted at all.
	struct Component
	{
		std::string name;
		bool enabled = true;
		std::vector<std::string> patterns;
	};
	std::vector<Component> components;
};

// Sweeps `trees` for `variant` and returns what they contribute, tree by tree
// and sorted by destination within each.
//
// Throws when a tree's directory is missing, when a swept file cannot be named
// in a ROM, or when two trees claim one destination.
[[nodiscard]] std::vector<config::FileConfig> sweepFileTrees(
	const std::vector<FileTree>& trees, std::string_view variant);

} // namespace ncp::rom
