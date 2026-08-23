#pragma once

// Finding and reading a project's configuration.
//
// Two schemas are read here. v2 is the YAML one; v1 is the JSON one every
// existing project is written in, and it keeps working for the whole 2.x line
// -- there is no flag day, only a deprecation notice pointing at `migrate`.
// Both produce the same ProjectConfig, which is what keeps the rest of the
// program from having to know which it was.

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "project_config.hpp"

namespace ncp::config {

// --var NAME=VALUE, as given on the command line. These win over the file's own
// vars: entries of the same name, which is what lets one project file serve a
// machine whose reference tree lives somewhere unusual without being edited.
using VarOverrides = std::vector<std::pair<std::string, std::string>>;

// Locates the configuration file in `projectRoot`. A v2 ncpatcher.yaml wins
// over a v1 ncpatcher.json when both are present, so a migrated project stops
// reading the old file without anyone having to delete it first.
// Returns an empty path when there is none.
[[nodiscard]] std::filesystem::path findProjectFile(const std::filesystem::path& projectRoot);

// Reads `projectFile`, dispatching on its schema. A document with no `version:`
// key is v1.
[[nodiscard]] ProjectConfig load(const std::filesystem::path& projectFile,
                                 const std::filesystem::path& projectRoot,
                                 const VarOverrides& varOverrides = {});

// The two readers, reachable directly because `migrate` needs the v1 one
// specifically and the tests need both.
struct V1Options
{
	// Rewrites ${env:X} into the v2 spelling ${env.X} and leaves it there
	// instead of substituting the value. Only `migrate` wants this: baking
	// somebody's local NSMBREF_ROOT into a config that is about to be committed
	// is precisely what the environment reference exists to avoid.
	bool keepEnvironmentReferences = false;

	// Suppresses deprecation notices. Set for the second of migrate's two
	// reads, which would otherwise say everything twice.
	bool quiet = false;

	VarOverrides varOverrides;
};

[[nodiscard]] ProjectConfig loadV1(const std::filesystem::path& projectFile,
                                   const std::filesystem::path& projectRoot,
                                   const V1Options& options = {});

// Reads the target files a v1 project points at, which load() deliberately
// leaves alone; see TargetConfig::deferred. A v2 project has none, so this does
// nothing there and can be called unconditionally.
void loadTargets(ProjectConfig& config, const V1Options& options = {});
[[nodiscard]] ProjectConfig loadV2(const std::filesystem::path& projectFile,
                                   const std::filesystem::path& projectRoot,
                                   const VarOverrides& varOverrides = {});

// Translates one v1 `includes`/`sources` entry of the old [path, recursive]
// pair form into the glob patterns that mean the same thing. Shared with
// `migrate`, which must emit exactly what the v1 reader would have matched.
void legacyPathPairToPatterns(const std::string& path, bool recursive, bool directoriesOnly,
                              std::vector<std::string>& out);

} // namespace ncp::config
