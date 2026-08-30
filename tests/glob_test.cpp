// Tests for source/utils/glob.{hpp,cpp}.
//
// Builds its own temporary tree so the test is self-contained.
// Run via ctest, or directly: ./glob_test

#include "../source/utils/glob.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

static int g_failures = 0;

static void check(bool condition, const std::string& what)
{
	if (!condition)
	{
		std::cout << "FAIL: " << what << "\n";
		g_failures++;
	}
}

static void expectMatch(const char* pattern, const char* path, bool expected)
{
	const bool actual = Glob::match(pattern, path);
	check(actual == expected,
		std::string("match(\"") + pattern + "\", \"" + path + "\") == "
		+ (actual ? "true" : "false") + ", expected " + (expected ? "true" : "false"));
}

static void touch(const fs::path& path)
{
	fs::create_directories(path.parent_path());
	std::ofstream out(path);
	out << "// test\n";
}

static void testMatching()
{
	// Wildcards inside one segment.
	expectMatch("*.cpp", "a.cpp", true);
	expectMatch("*.cpp", "a.hpp", false);
	expectMatch("a?c", "abc", true);
	expectMatch("a?c", "ac", false);

	// Character classes.
	expectMatch("[abc].c", "b.c", true);
	expectMatch("[!abc].c", "b.c", false);
	expectMatch("[!abc].c", "d.c", true);
	expectMatch("[^abc].c", "d.c", true);
	expectMatch("[a-z].c", "q.c", true);
	expectMatch("[a-z].c", "Q.c", false);

	// '*' must not cross a separator.
	expectMatch("src/*", "src/a.cpp", true);
	expectMatch("src/*", "src/sub/b.cpp", false);

	// '**' spans zero or more segments. The previous hand-rolled matcher
	// silently matched nothing here, so a project using the documented
	// "source/**" syntax compiled no sources at all.
	expectMatch("src/**", "src/a.cpp", true);
	expectMatch("src/**", "src/sub/deep/c.cpp", true);
	expectMatch("src/**/*.cpp", "src/a.cpp", true);
	expectMatch("src/**/*.cpp", "src/sub/deep/c.cpp", true);
	expectMatch("src/**/*.cpp", "src/sub/deep/c.hpp", false);
	expectMatch("**", "anything/at/all", true);
	expectMatch("a/**/b", "a/b", true);
	expectMatch("a/**/b", "a/x/y/b", true);
	expectMatch("a/**/b", "a/x/y/c", false);

	// Brace alternation.
	expectMatch("*.{c,cpp}", "a.cpp", true);
	expectMatch("*.{c,cpp}", "a.c", true);
	expectMatch("*.{c,cpp}", "a.s", false);
	check(Glob::expandBraces("a{1,2}b").size() == 2, "expandBraces yields 2");
	check(Glob::expandBraces("a{1,{2,3}}b").size() == 3, "nested braces yield 3");

	// Backslashes are accepted as separators.
	expectMatch("src/**", "src\\sub\\b.cpp", true);
}

static void testExpansion(const fs::path& root)
{
	// Everything here goes through an explicit baseDir, which is how the build
	// calls it, and nothing may depend on the process working directory.
	auto count = [&](const char* pattern, bool directoriesOnly) {
		Glob::Options options;
		options.directoriesOnly = directoriesOnly;
		return Glob::expand(pattern, root, options).size();
	};

	check(count("src/**", false) == 3, "src/** finds all 3 files");
	check(count("src/*", false) == 1, "src/* finds only the top-level file");
	check(count("src/**/*.cpp", false) == 3, "src/**/*.cpp finds 3");
	check(count("src", false) == 1, "literal directory yields its direct files");
	check(count("src/**", true) == 2, "src/** in directory mode finds 2 dirs");
	check(count("inc", true) == 1, "literal directory in directory mode");
	check(count("does/not/exist", false) == 0, "missing literal yields nothing");
	check(count("src/*.nope", false) == 0, "non-matching pattern yields nothing");

	// Relative patterns must stay relative even though baseDir is absolute:
	// object paths are derived from the source path, so prepending baseDir here
	// would relocate build output.
	const std::vector<fs::path> relative = Glob::expand("src/**", root, {});
	check(relative.size() == 3, "baseDir search finds all 3 files");
	check(!relative.empty() && relative.front().is_relative(), "relative in, relative out");

	// ...and they must name a real file once anchored back onto baseDir.
	for (const fs::path& p : relative)
		check(fs::exists(root / p), "result resolves under baseDir");

	// The base is a search root only, never the process working directory.
	const fs::path elsewhere = fs::temp_directory_path();
	check(Glob::expand("src/**", elsewhere, {}).empty(), "a different base finds nothing");

	// Absolute patterns must resolve too (projects use ${env:...}/include), and
	// come back absolute, since they cannot be expressed relative to baseDir.
	const std::vector<fs::path> absolute =
		Glob::expand((root / "src" / "**").generic_string(), elsewhere, {});
	check(absolute.size() == 3, "absolute pattern resolves regardless of base");
	check(!absolute.empty() && absolute.front().is_absolute(), "absolute in, absolute out");

	// Deterministic ordering: directory_iterator order is unspecified.
	check(relative == Glob::expand("src/**", root, {}), "expansion is stable");
	check(std::is_sorted(relative.begin(), relative.end()), "expansion is sorted");
}

int main()
{
	const fs::path root = fs::temp_directory_path() / "ncp_glob_test";
	fs::remove_all(root);
	touch(root / "src" / "a.cpp");
	touch(root / "src" / "sub" / "b.cpp");
	touch(root / "src" / "sub" / "deep" / "c.cpp");
	fs::create_directories(root / "inc");

	testMatching();
	testExpansion(root);

	fs::remove_all(root);

	if (g_failures != 0)
	{
		std::cout << g_failures << " failure(s)\n";
		return 1;
	}
	std::cout << "glob_test: all checks passed\n";
	return 0;
}
