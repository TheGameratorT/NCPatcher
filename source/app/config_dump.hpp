#pragma once

// Printing the configuration as the build actually understands it.
//
// A project's settings arrive from four places and pass through three levels of
// inheritance, a variable expander and a glob matcher before anything is
// compiled. When the result is not what someone expected, the useful question
// is not "what does the file say" -- they can read that -- but "what did you
// make of it, and which line won". That is what this answers.
//
// The JSON form is also the equivalence check between the two schemas: a v1
// project and the same project after `migrate` must dump the same thing, which
// is a far more diagnostic comparison than the ROM bytes, because it names the
// setting that differs instead of the offset.

#include <ostream>
#include <vector>

#include "context.hpp"
#include "../config/buildtarget.hpp"
#include "../config/project_config.hpp"

namespace ncp {

struct ResolvedTarget
{
	const config::TargetConfig* config = nullptr;
	BuildTarget target;
};

struct DumpOptions
{
	bool json = false;
	// Annotate each setting with where it came from.
	bool explain = false;
};

void dumpConfig(std::ostream& out,
                const config::ProjectConfig& config,
                const PathContext& paths,
                const std::vector<ResolvedTarget>& targets,
                const DumpOptions& options);

} // namespace ncp
