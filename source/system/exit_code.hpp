#pragma once

// What the process returns, and why.
//
// A caller that only ever sees 0 or 1 has to parse the log to find out what
// went wrong, which is exactly what NSMB-Editor does today, and why it can
// only ever put up a "compilation failed" box. The categories below are coarse
// on purpose: fine-grained identification is the NCPxxxx code's job, and lives
// in the diagnostic stream. This is for shell scripts and `if` statements.

#include "diagnostics.hpp"

namespace ncp {

enum class ExitCode : int
{
	Ok          = 0,
	Internal    = 1,   // a failure with no context attached: a bug here
	Usage       = 2,   // the command line did not parse
	Config      = 3,
	Modules     = 4,
	Toolchain   = 5,
	Compile     = 6,
	Link        = 7,
	Patch       = 8,
	RomIo       = 9,
	Hook        = 10,  // a pre-build or post-build command failed

	// Someone asked the build to stop, and it did. Not a failure: nothing is
	// wrong with the project and nothing has to be fixed before the next run.
	// See system/cancel.hpp for what to send and where it takes effect.
	Cancelled   = 11,

	// 128 + SIGINT, the shell convention. Reserved rather than returned: a
	// Ctrl-C today terminates the process through the default disposition, and
	// the shell reports this value itself. It is named here so that a handler
	// added later cannot pick a different number.
	Interrupted = 130,
};

// The category a failure in this phase belongs to. Diag::None maps to
// Internal, since a throw outside every context is one nobody anticipated.
[[nodiscard]] ExitCode exitCodeFor(Diag code);

[[nodiscard]] inline int exitValue(ExitCode code) { return static_cast<int>(code); }

} // namespace ncp
