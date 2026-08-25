#include "file_tree.hpp"

#include <algorithm>
#include <sstream>
#include <map>

#include "../system/except.hpp"
#include "../system/log.hpp"
#include "../utils/glob.hpp"

namespace fs = std::filesystem;

namespace ncp::rom {

namespace {

// How specific a layer is to the variant being built. The base layer applies to
// every variant, so a file chosen from it is the weaker claim.
enum Rank
{
	RANK_BASE = 0,
	RANK_VARIANT = 1
};

// One destination's current winner, with enough about the claim to decide
// whether a later one displaces it.
struct Resolved
{
	config::FileConfig file;
	int rank = RANK_BASE;
	std::size_t tree = 0;
};

std::string relativePosix(const fs::path& file, const fs::path& root)
{
	return fs::relative(file, root).generic_string();
}

// Every regular file under `root`, sorted, so that two machines walking the
// same directory produce the same order and therefore the same file ids.
std::vector<fs::path> filesUnder(const fs::path& root)
{
	std::vector<fs::path> out;
	std::error_code error;
	fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, error);
	if (error)
		throw ncp::file_error(root, ncp::file_error::read);

	for (const fs::directory_entry& entry : it)
	{
		if (entry.is_regular_file(error))
			out.push_back(entry.path());
	}

	std::sort(out.begin(), out.end());
	return out;
}

// Rewrites the `_narc` directory convention into an archive destination.
//
// A directory cannot also be a file, so a project that wants to keep the file
// it is replacing inside an archive next to the rest of its assets has to spell
// the archive as a folder. The established spelling is the archive's name with
// its dot turned into an underscore, and everything below that folder is a path
// inside it:
//
//     ARCHIVE/menu_title_narc/menu/title/USA/vs.bmg
//       -> ARCHIVE/menu_title.narc!menu/title/USA/vs.bmg
//
// Only the first such segment is read that way: an archive inside an archive is
// not something this opens. A ROM that genuinely contains a directory named
// `*_narc` cannot be expressed by a tree, and has to be written in `files:`
// instead, where no convention applies.
std::string applyNarcConvention(const std::string& destination)
{
	constexpr std::string_view SUFFIX = "_narc";

	std::size_t start = 0;
	while (start < destination.size())
	{
		const std::size_t slash = destination.find('/', start);
		if (slash == std::string::npos)
			break;

		const std::string_view segment(destination.data() + start, slash - start);
		if (segment.size() > SUFFIX.size() && segment.ends_with(SUFFIX))
		{
			const std::size_t stem = start + segment.size() - SUFFIX.size();
			return destination.substr(0, stem) + ".narc!" + destination.substr(slash + 1);
		}

		start = slash + 1;
	}
	return destination;
}

// The component whose patterns claim `destination`, or nullptr. First match
// wins, in the module's declaration order, so a component can be read as
// owning a prefix of the tree.
const FileTree::Component* claimingComponent(const FileTree& tree, const std::string& destination)
{
	for (const FileTree::Component& component : tree.components)
	{
		for (const std::string& pattern : component.patterns)
		{
			for (const std::string& expanded : Glob::expandBraces(pattern))
			{
				if (Glob::match(expanded, destination))
					return &component;
			}
		}
	}
	return nullptr;
}

} // namespace

std::vector<config::FileConfig> sweepFileTrees(
	const std::vector<FileTree>& trees, std::string_view variant)
{
	for (const FileTree& tree : trees)
	{
		if (!fs::is_directory(tree.dir))
		{
			std::ostringstream oss;
			oss << "The NitroFS tree of " << OSTR(tree.origin) << " is not a directory: "
			    << OSTR(tree.dir.string()) << ".";
			throw ncp::exception(oss.str());
		}
	}

	// Ordered, so the result does not depend on hashing.
	std::map<std::string, Resolved> chosen;

	// The base layer of every tree first, then the variant layer of every tree.
	// The two loops are what make layering outrank module order: a module that
	// translates a file for one language beats a module that supplies the same
	// path for all of them, whichever of the two is declared first.
	for (const int rank : { RANK_BASE, RANK_VARIANT })
	{
		for (std::size_t treeIndex = 0; treeIndex < trees.size(); treeIndex++)
		{
			const FileTree& tree = trees[treeIndex];

			// An unlayered tree applies to every variant, so it is a base layer
			// and has nothing to contribute to the variant pass.
			std::string layer;
			if (tree.layered)
			{
				layer = (rank == RANK_BASE)
					? tree.baseVariant
					: tree.variantLayer.value_or(std::string(variant));
			}
			else if (rank != RANK_BASE)
			{
				continue;
			}

			if (tree.layered && layer.empty())
				continue;
			if (rank == RANK_VARIANT && layer == tree.baseVariant)
				continue;

			const fs::path root = tree.layered ? tree.dir / layer : tree.dir;

			if (!fs::is_directory(root))
			{
				// A layer the project named explicitly has to be there; see
				// FileTree::variantLayer.
				if (rank == RANK_VARIANT && tree.variantLayer.has_value())
				{
					std::ostringstream oss;
					oss << OSTRa(tree.origin) << " has no " << OSTR(layer)
					    << " in its NitroFS tree." << OREASONNL
					    << OSTR(root.string()) << " does not exist.";
					throw ncp::exception(oss.str());
				}

				// Otherwise ordinary: the common case is a module that ships
				// one language's assets and lets the base layer answer for the
				// other eight.
				continue;
			}

			for (const fs::path& source : filesUnder(root))
			{
				const std::string relative = relativePosix(source, root);
				const std::string destination = applyNarcConvention(tree.into.empty()
					? relative : tree.into + "/" + relative);

				if (const std::string problem = config::nitroDestinationProblem(destination); !problem.empty())
				{
					std::ostringstream oss;
					oss << "Cannot insert " << OSTR(source.string()) << " as "
					    << OSTR(destination) << "." << OREASONNL << problem;
					throw ncp::exception(oss.str());
				}

				const FileTree::Component* component = claimingComponent(tree, destination);
				if (component != nullptr && !component->enabled)
					continue;

				// Within one layer the first tree to claim a destination keeps
				// it, and `modules.enabled` is the order. That is a precedence
				// rule rather than an accident: a module which replaces a piece
				// of artwork wholesale has to outrank one that only translates
				// the stock version of it, and the project says which is which
				// by listing them in that order.
				const auto existing = chosen.find(destination);
				if (existing != chosen.end() && existing->second.rank >= rank)
					continue;

				config::FileConfig file;
				file.path = destination;
				file.source = source;
				file.module = tree.module;
				file.component = component == nullptr ? std::string() : component->name;
				file.fromVariant = layer;

				chosen[destination] = Resolved{ std::move(file), rank, treeIndex };
			}
		}
	}

	std::vector<config::FileConfig> out;
	out.reserve(chosen.size());
	for (auto& [destination, resolved] : chosen)
		out.push_back(std::move(resolved.file));
	return out;
}

} // namespace ncp::rom
