#pragma once

#include <filesystem>

namespace ncp {

// Absolute anchors that relative paths in a build resolve against.
//
// These used to be the process-wide current directory: every phase switched it
// to whatever it needed and switched it back on the way out. That made the
// meaning of a relative path depend on which phase happened to run last, leaked
// the wrong directory into later phases whenever one threw, and could never
// survive running phases concurrently. Resolve against one of these instead.
struct PathContext
{
	// Project directory, holding ncpatcher.json. The project config's relative
	// paths -- backup dir, filesystem dir, build dirs -- are relative to this.
	std::filesystem::path workDir;

	// Extracted ROM filesystem. arm9.bin, the overlay tables and the overlay
	// binaries are named relative to this.
	std::filesystem::path romDir;

	// Directory the target config resolves against: its includes, its sources,
	// and the object paths derived from them. Empty until a target is loaded.
	std::filesystem::path targetWorkDir;

	// Build output directory of the target being processed. Empty until then.
	std::filesystem::path buildDir;

	[[nodiscard]] std::filesystem::path work(const std::filesystem::path& p) const { return resolve(workDir, p); }
	[[nodiscard]] std::filesystem::path rom(const std::filesystem::path& p) const { return resolve(romDir, p); }
	[[nodiscard]] std::filesystem::path target(const std::filesystem::path& p) const { return resolve(targetWorkDir, p); }

private:
	// Config values are allowed to be absolute -- an ${env:...} expansion usually
	// is -- and such a path must not be re-anchored. operator/ already does this,
	// but spelling it out keeps it a deliberate rule rather than a side effect.
	[[nodiscard]] static std::filesystem::path resolve(
		const std::filesystem::path& base, const std::filesystem::path& p)
	{
		return p.is_absolute() ? p : base / p;
	}
};

} // namespace ncp
