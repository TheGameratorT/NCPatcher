#pragma once

// Turning `modules:` plus a directory of module.yaml files into a ModuleGraph.
//
// Every step reports as much as it can before giving up. A project enables a
// dozen modules at once; a resolver that stopped at the first mistake would
// turn one bad afternoon of edits into a dozen builds.
//
// It needs no toolchain, no ROM and no target configuration, only the project
// directory, which is what lets `modules dump` run in a checkout that has
// never been built, and what lets a build write the dump before its pre-build
// commands run.

#include <filesystem>

#include "module_graph.hpp"
#include "../config/project_config.hpp"

namespace ncp::modules {

struct ResolveOptions
{
	// Suppresses warnings that a second resolve would only repeat.
	bool quiet = false;
};

// Throws ncp::exception listing every problem found. Returns an empty graph
// when the project has no `modules:` section.
[[nodiscard]] ModuleGraph resolve(const config::ModulesConfig& config,
                                  const std::filesystem::path& workDir,
                                  const ResolveOptions& options = {});

} // namespace ncp::modules
