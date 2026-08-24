#pragma once

// Turning a parsed configuration into a BuildTarget.
//
// This is where the three-level inheritance actually happens, where ${...} has
// already stopped existing, and where glob patterns become file lists. It is
// also the single place that decides what a flag list means once it reaches a
// command line, which is why the same resolver serves both schema versions:
// the v1 reader and the v2 reader disagree about syntax, not about outcome.

#include <string>
#include <vector>

#include "buildtarget.hpp"
#include "project_config.hpp"
#include "../modules/module_graph.hpp"
#include "../system/path_context.hpp"

namespace ncp::config {

namespace TargetResolver {

struct Options
{
	// Suppresses the "pattern matched nothing" warnings. Set when a resolve is
	// a self-check rather than a build -- `migrate` resolves both schemas to
	// compare them, and reporting the same unmatched pattern twice would only
	// make the real output harder to read.
	bool quiet = false;
};

// Applies project -> target -> region inheritance, expands the include and
// source globs against `paths.targetWorkDir`, and joins each flag list into the
// string the compiler driver is handed.
//
// `graph` folds in what the enabled modules contribute: their include
// directories, their defines, and the sources they put in each region. It may
// be null, which is what a project without a `modules:` section resolves as.
[[nodiscard]] BuildTarget resolve(const ProjectConfig& config,
                                  const TargetConfig& target,
                                  const PathContext& paths,
                                  const modules::ModuleGraph* graph = nullptr,
                                  const Options& options = {});

// A canonical, line-per-setting rendering of everything that determines how
// this target's objects get compiled and laid out.
//
// It is what the config hash is taken over, and what `config dump` prints. The
// expanded source file list is deliberately not in it: adding a source file
// changes what gets compiled but not how, and the per-file dependency tracking
// already handles that. Making it part of the hash would turn every new file
// into a full rebuild.
[[nodiscard]] std::string describe(const ProjectConfig& config, const BuildTarget& resolved);

// The project-wide half of the same question: settings that are not part of any
// one target but still invalidate what is on disk when they change.
[[nodiscard]] std::string describeProject(const ProjectConfig& config,
                                          const std::vector<std::string>& commandLineDefines);

} // namespace TargetResolver

} // namespace ncp::config
