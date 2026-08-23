#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Glob {

/**
 * Glob matching and expansion.
 *
 * Supported syntax:
 *   *         matches any run of characters within one path segment
 *   ?         matches a single character within one path segment
 *   [abc]     character class; [!abc] or [^abc] negates; [a-z] ranges
 *   {a,b}     brace alternation, expanded before matching (may nest)
 *   **        as a whole segment, matches zero or more path segments
 *   !pattern  as a whole pattern, excludes previously matched entries
 *
 * Separators are '/'; on Windows '\' is accepted and normalised.
 */

struct Options
{
	/// Collect directories instead of regular files.
	bool directoriesOnly = false;
};

/// Expand '{a,b}' alternations into the equivalent list of brace-free patterns.
[[nodiscard]] std::vector<std::string> expandBraces(std::string_view pattern);

/// Match one brace-free pattern against a '/'-separated relative path.
[[nodiscard]] bool match(std::string_view pattern, std::string_view path);

/**
 * Expand a pattern against the filesystem, searching under baseDir (the current
 * directory when it is empty).
 *
 * Results are returned as the pattern wrote them: relative to baseDir for a
 * relative pattern, absolute for an absolute one. baseDir is never prepended --
 * callers that need absolute paths join it themselves. This keeps source paths
 * relative, which is what the object-file layout is derived from.
 *
 * A pattern with no wildcard is treated as a literal path:
 *   - directoriesOnly: the directory itself is returned
 *   - otherwise:       a file is returned as-is; a directory yields the
 *                      regular files directly inside it (non-recursive)
 *
 * Results are returned sorted, so build output does not depend on the
 * unspecified iteration order of std::filesystem::directory_iterator.
 */
[[nodiscard]] std::vector<std::filesystem::path> expand(
	std::string_view pattern,
	const std::filesystem::path& baseDir,
	const Options& options);

/// True if the pattern contains any wildcard metacharacter.
[[nodiscard]] bool hasWildcard(std::string_view pattern);

} // namespace Glob
