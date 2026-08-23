// Tests for source/config/**.
//
// The configuration layer is the one part of NCPatcher whose mistakes are
// silent: a misread flag does not crash, it produces a ROM that is subtly
// wrong. So the cases here lean towards the things that used to go unnoticed --
// a size limit that quietly became the default, a define that reached one
// language and not another, a variable that could only be read if it was
// declared above its user.
//
// Run via ctest, or directly: ./config_test

#include "../source/config/config_loader.hpp"
#include "../source/config/expander.hpp"
#include "../source/config/migrate.hpp"
#include "../source/config/node.hpp"
#include "../source/config/rebuild_store.hpp"
#include "../source/config/target_resolver.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using namespace ncp;
using namespace ncp::config;
using cfg::Document;
using cfg::Node;

static int g_failures = 0;

static void check(bool condition, const std::string& what)
{
	if (!condition)
	{
		std::cout << "FAIL: " << what << "\n";
		g_failures++;
	}
}

static void checkEqual(const std::string& actual, const std::string& expected, const std::string& what)
{
	if (actual != expected)
	{
		std::cout << "FAIL: " << what << "\n  expected: " << expected << "\n  actual:   " << actual << "\n";
		g_failures++;
	}
}

// Runs `body` and returns the error message it threw, or "" if it did not.
template <typename F>
static std::string errorFrom(F&& body)
{
	try {
		body();
	} catch (const std::exception& e) {
		return e.what();
	}
	return {};
}

static void write(const fs::path& path, const std::string& text)
{
	fs::create_directories(path.parent_path());
	std::ofstream out(path, std::ios::binary);
	out << text;
}

static bool contains(const std::string& haystack, const std::string& needle)
{
	return haystack.find(needle) != std::string::npos;
}

// cfg::Node ==============================================================

static void testNodeReading()
{
	const Document doc("version: 2\nsizes:\n  small: 16\n  huge: \"0xFFFF0000\"\n  signed: -1\n", "t.yaml");
	const Node root = doc.root();

	check(root.isMap(), "a mapping parses as one");
	check(root["version"].asInt() == 2, "an integer reads back");
	check(root["sizes"]["small"].asU32() == 16, "a decimal reads as u32");

	// getInt() returned int, so every address at or above 0x80000000 was
	// undefined behaviour rather than a diagnostic.
	check(root["sizes"]["huge"].asU32() == 0xFFFF0000u, "a hex string above 2^31 reads as u32");
	check(!errorFrom([&] { (void)root["sizes"]["signed"].asU32(); }).empty(), "a negative is not a u32");

	check(!root["nothing"].defined(), "a missing key is undefined, not an error");
	check(!root["version"]["nested"].defined(), "indexing a scalar yields undefined, not a throw");
	check(root["nothing"].asU32(7) == 7, "a missing key falls back");
}

static void testNodeErrorsPointAtTheLine()
{
	const Document doc("targets:\n  arm9:\n    mode: appned\n", "ncpatcher.yaml");
	const std::string error = errorFrom([&] {
		doc.root()["targets"]["arm9"]["mode"].fail("Invalid mode.");
	});

	check(contains(error, "ncpatcher.yaml:3:11"), "the error carries file, line and column");
	check(contains(error, "targets.arm9.mode"), "the error names the setting");
	check(contains(error, "mode: appned"), "the error quotes the source line");
	check(contains(error, "^"), "the error points at the column");
}

static void testNodeRejectsMissingRequirement()
{
	const Document doc("a: 1\n", "t.yaml");
	const std::string error = errorFrom([&] { (void)doc.root().require("b"); });
	check(contains(error, "\"b\""), "a required key that is missing says which");
}

static void testParseErrorIsPositioned()
{
	const std::string error = errorFrom([] { const Document doc("a: [1,\nb: :\n", "broken.yaml"); (void)doc; });
	check(!error.empty(), "a syntax error is reported");
	check(contains(error, "broken.yaml:"), "a syntax error names the file and position");
}

// ListOp and DefineSet ===================================================

static void testListOps()
{
	const std::vector<std::string> inherited = { "-Os", "-g", "-Wall" };

	ListOp append;
	append.append = { "-O2" };
	check(append.applyTo(inherited) == std::vector<std::string>({ "-Os", "-g", "-Wall", "-O2" }),
		"a bare list appends");

	ListOp remove;
	remove.remove = { "-Os" };
	remove.append = { "-O2" };
	check(remove.applyTo(inherited) == std::vector<std::string>({ "-g", "-Wall", "-O2" }),
		"remove runs before append");

	ListOp replace;
	replace.hasSet = true;
	replace.set = { "-O0" };
	check(replace.applyTo(inherited) == std::vector<std::string>({ "-O0" }), "set discards the inherited list");

	ListOp clear;
	clear.hasSet = true;
	check(clear.applyTo(inherited).empty(), "an explicit empty set means nothing, not no-op");
}

static void testDefineSet()
{
	DefineSet defines;
	defines.add("SDK_GCC", "project");
	defines.add("VALUE=1", "project");
	defines.add("SDK_GCC", "target");

	check(defines.entries().size() == 2, "a repeated name does not duplicate");
	checkEqual(defines.toFlags()[1], "-DVALUE=1", "a valued define renders with its value");

	defines.add("VALUE=2", "target");
	check(defines.find("VALUE")->value == "2", "a later define overrides");
	check(defines.entries()[1].name == "VALUE", "overriding keeps the original position");

	defines.remove("VALUE");
	check(defines.find("VALUE") == nullptr, "a define can be removed");
}

// Expander ===============================================================

static void testExpanderIsLazy()
{
	const Document doc("a: x\n", "t.yaml");
	const Node origin = doc.root()["a"];

	Expander expander;
	// Declared in the order that v1 could not have read: `first` refers to
	// `second`, which is declared after it.
	expander.setVariable("first", "${vars.second}/include", origin);
	expander.setVariable("second", "/opt/ref", origin);

	checkEqual(expander.expand("${vars.first}", origin), "/opt/ref/include",
		"a variable may refer to one declared below it");
}

static void testExpanderDetectsCycles()
{
	const Document doc("a: x\n", "t.yaml");
	const Node origin = doc.root()["a"];

	Expander expander;
	expander.setVariable("a", "${vars.b}", origin);
	expander.setVariable("b", "${vars.a}", origin);

	const std::string error = errorFrom([&] { (void)expander.expand("${vars.a}", origin); });
	check(contains(error, "refers to itself"), "a cycle is reported as a cycle");
	check(contains(error, "vars.a -> vars.b"), "the cycle error shows the loop");
}

static void testExpanderEnvironment()
{
	const Document doc("a: x\n", "t.yaml");
	const Node origin = doc.root()["a"];
	Expander expander;
	expander.setConstant("project.root", "/proj");

	checkEqual(expander.expand("${project.root}/src", origin), "/proj/src", "a constant expands");
	checkEqual(expander.expand("${env.NCP_DEFINITELY_UNSET:-fallback}", origin), "fallback",
		"an unset environment variable takes its fallback");

	const std::string error = errorFrom([&] { (void)expander.expand("${env.NCP_DEFINITELY_UNSET}", origin); });
	check(contains(error, "is not set"), "an unset environment variable with no fallback is an error");
	check(contains(error, ":-default"), "the error suggests the fallback syntax");

	checkEqual(expander.expand("cost: $$5 and $HOME", origin), "cost: $5 and $HOME",
		"$$ escapes, and a lone $ is literal");

	const std::string unknown = errorFrom([&] { (void)expander.expand("${ref}", origin); });
	check(contains(unknown, "${vars.ref}"), "a bare name is pointed at the vars namespace");
}

// v1 reader ==============================================================

static const char* V1_PROJECT = R"({
	"$arm_flags": "-mabi=aapcs",
	"$c_flags": "${arm_flags} -Os -DSDK_GCC",
	"backup": "backup",
	"filesystem": "__tmp",
	"toolchain": "arm-none-eabi-",
	"arm7": {},
	"arm9": { "target": "code/arm9.json", "build": "code/build" },
	"pre-build": [],
	"post-build": [],
	"thread-count": 0
})";

static const char* V1_TARGET = R"({
	"$c_flags": "$${c_flags} -DSDK_ARM9",
	"c_flags": "${c_flags}",
	"cpp_flags": "${c_flags} -std=c++23",
	"asm_flags": "-mabi=aapcs -x assembler-with-cpp",
	"ld_flags": "-lgcc,-lc",
	"includes": [["include", false], "generated"],
	"regions": [{
		"dest": "main",
		"compress": false,
		"sources": [["source", true]]
	}, {
		"dest": "ov9",
		"length": "0x56400",
		"compress": false,
		"sources": ["source/ov9"]
	}]
})";

static ProjectConfig loadV1Fixture(const fs::path& root)
{
	write(root / "ncpatcher.json", V1_PROJECT);
	write(root / "code" / "arm9.json", V1_TARGET);
	fs::create_directories(root / "code" / "include");
	fs::create_directories(root / "code" / "generated");
	write(root / "code" / "source" / "a.cpp", "// a\n");
	write(root / "code" / "source" / "deep" / "b.cpp", "// b\n");
	write(root / "code" / "source" / "ov9" / "c.cpp", "// c\n");

	V1Options quiet;
	quiet.quiet = true;
	ProjectConfig config = loadV1(root / "ncpatcher.json", root, quiet);
	loadTargets(config, quiet);
	return config;
}

static void testV1Reader(const fs::path& root)
{
	const ProjectConfig config = loadV1Fixture(root);

	check(config.version == 1, "a document with no version key is v1");
	check(!config.arm7.enabled, "an empty target object means the target is off");
	check(config.arm9.enabled, "a populated target object means it is on");
	checkEqual(config.toolchain.value, "arm-none-eabi-", "the toolchain reads back");

	// The [path, recursive] pair form: six shipped projects still use it, and
	// the glob rewrite would have thrown on every one of them.
	const std::vector<std::string> includes = config.arm9.includes.applyTo({});
	check(includes == std::vector<std::string>({ "include", "generated" }),
		"a [path, false] include pair becomes the directory itself");

	// `length` was renamed to `maxsize` without an alias, so every project
	// still saying it silently fell back to the 1 MiB default.
	check(config.arm9.regions[1].maxsize.configured(), "length is still read");
	check(config.arm9.regions[1].maxsize.value == 0x56400, "length reads as maxsize");

	PathContext paths;
	paths.workDir = root;
	paths.targetWorkDir = root / "code";
	TargetResolver::Options quiet;
	quiet.quiet = true;
	const BuildTarget resolved = TargetResolver::resolve(config, config.arm9, paths, quiet);

	// v1 flag strings are opaque; resolving must not reformat them.
	checkEqual(resolved.cFlags, "-mabi=aapcs -Os -DSDK_GCC -DSDK_ARM9",
		"v1 flag strings survive resolution unchanged");
	checkEqual(resolved.ldFlags, "-lgcc,-lc", "v1 ld flags round-trip through the list form");

	check(resolved.regions.size() == 2, "both regions resolved");
	// source/**: a.cpp, deep/b.cpp and ov9/c.cpp.
	check(resolved.regions[0].sources.size() == 3, "a [path, true] source pair recurses");
	check(resolved.regions[1].maxsize == 0x56400, "the resolved region carries the size limit");
	check(resolved.regions[0].maxsize == 0x100000, "a region with no limit gets the default");
}

// v2 reader ==============================================================

static const char* V2_PROJECT = R"(version: 2
vars:
  gen: ${project.root}/generated

rom:
  dir: __tmp
  backup: backup

toolchain: { prefix: arm-none-eabi- }

defines: [SDK_GCC]
flags:
  common: [-mabi=aapcs]
  c: [-Os]
  cpp: [-Os, -std=c++23]
  asm: [-x assembler-with-cpp]
  ld: [-lgcc, -lc]

targets:
  arm7: { enabled: false }
  arm9:
    build: code/build
    workdir: code
    arena-lo: 0x02065F10
    defines: [SDK_ARM9]
    includes: [include]
    flags:
      common: [-march=armv5te]
    regions:
      - dest: main
        sources: ["source/**", "!source/deep/**"]
      - dest: ov9
        maxsize: 0x56400
        defines: [OVERLAY_ID=9]
        sources: [source/ov9]
        flags:
          cpp: { remove: [-Os], append: [-O2] }
)";

static void testV2Reader(const fs::path& root)
{
	write(root / "ncpatcher.yaml", V2_PROJECT);
	const ProjectConfig config = loadV2(root / "ncpatcher.yaml", root);

	check(config.version == 2, "the version key is read");
	check(!config.arm7.enabled, "enabled: false turns a target off");
	checkEqual(config.filesystemDir.value.generic_string(), "__tmp", "rom.dir reads back");

	PathContext paths;
	paths.workDir = root;
	paths.targetWorkDir = root / "code";
	TargetResolver::Options quiet;
	quiet.quiet = true;
	const BuildTarget resolved = TargetResolver::resolve(config, config.arm9, paths, quiet);

	check(resolved.arenaLo == 0x02065F10, "arena-lo reads as an address");

	// Inheritance: project common, then target common, then the language list,
	// then the defines -- which reach assembly too.
	checkEqual(resolved.regions[0].cFlags,
		"-mabi=aapcs -march=armv5te -Os -DSDK_GCC -DSDK_ARM9",
		"a region inherits project and target flags in order");
	checkEqual(resolved.regions[0].asmFlags,
		"-mabi=aapcs -march=armv5te -x assembler-with-cpp -DSDK_GCC -DSDK_ARM9",
		"defines reach the assembler as well");

	// The explicit operation form, which is the escape hatch from append.
	checkEqual(resolved.regions[1].cppFlags,
		"-mabi=aapcs -march=armv5te -std=c++23 -O2 -DSDK_GCC -DSDK_ARM9 -DOVERLAY_ID=9",
		"a region can remove an inherited flag and add its own");

	checkEqual(resolved.ldFlags, "-lgcc,-lc", "ld flags join with commas for -Wl");

	// An exclusion applies after everything has matched, wherever it appears.
	std::vector<std::string> mainSources;
	for (const fs::path& source : resolved.regions[0].sources)
		mainSources.push_back(source.generic_string());
	std::sort(mainSources.begin(), mainSources.end());
	check(mainSources == std::vector<std::string>({ "source/a.cpp", "source/ov9/c.cpp" }),
		"a !pattern excludes from what the other patterns matched");
}

static void testV2RejectsTypos(const fs::path& root)
{
	const std::string bad = std::string(V2_PROJECT).replace(
		std::string(V2_PROJECT).find("maxsize"), 7, "maxsze");
	write(root / "typo.yaml", bad);

	const std::string error = errorFrom([&] { (void)loadV2(root / "typo.yaml", root); });
	check(contains(error, "maxsze"), "an unknown key is named");
	check(contains(error, "Expected one of"), "an unknown key lists what was expected");

	// The reserved sections belong to phases that do not exist yet; saying so
	// beats "unknown key" when someone tries a config written for a later one.
	write(root / "future.yaml", std::string(V2_PROJECT) + "\nmodules:\n  dir: modules\n");
	const std::string reserved = errorFrom([&] { (void)loadV2(root / "future.yaml", root); });
	check(contains(reserved, "not supported by this version"), "a reserved section says so");
}

// Migration ==============================================================

static void testMigrationPreservesTheBuild(const fs::path& root)
{
	const ProjectConfig before = loadV1Fixture(root);

	// migrate() throws when the conversion does not resolve identically, so
	// reaching the end is itself the assertion. Writing is what proves the
	// emitted document is one the v2 reader accepts.
	const std::string error = errorFrom([&] {
		migrate(root / "ncpatcher.json", root, true);
	});
	checkEqual(error, "", "migration succeeds and verifies itself");
	check(fs::exists(root / "ncpatcher.yaml"), "migration writes ncpatcher.yaml");

	// findProjectFile prefers the new file, so the project switches over
	// without anyone having to delete the old one.
	checkEqual(findProjectFile(root).filename().string(), "ncpatcher.yaml",
		"a migrated project reads the v2 file");

	const ProjectConfig after = loadV2(root / "ncpatcher.yaml", root);

	PathContext paths;
	paths.workDir = root;
	paths.targetWorkDir = root / "code";
	TargetResolver::Options quiet;
	quiet.quiet = true;
	const BuildTarget resolvedBefore = TargetResolver::resolve(before, before.arm9, paths, quiet);
	const BuildTarget resolvedAfter = TargetResolver::resolve(after, after.arm9, paths, quiet);

	check(resolvedBefore.regions.size() == resolvedAfter.regions.size(), "region count survives");
	check(resolvedBefore.regions[1].maxsize == resolvedAfter.regions[1].maxsize,
		"the size limit survives migration");
	checkEqual(resolvedAfter.ldFlags, resolvedBefore.ldFlags, "ld flags survive migration");

	// The one thing migration deliberately changes: -D switches that v1 gave
	// only to C now reach the assembler too.
	check(contains(resolvedAfter.regions[0].asmFlags, "-DSDK_ARM9"),
		"migrated defines reach the assembler");

	// A second run must not silently overwrite the first.
	const std::string again = errorFrom([&] { migrate(root / "ncpatcher.json", root, true); });
	check(contains(again, "already exists"), "migration refuses to overwrite an existing yaml");
}

// RebuildStore ===========================================================

static void testRebuildStore(const fs::path& root)
{
	fs::create_directories(root);
	const fs::path file = root / "rebuild.json";

	{
		RebuildStore store;
		store.setProjectHash("abc");
		store.setTargetHash(true, "def");
		store.patchedOverlays(true) = { 8, 9, 12 };
		store.save(file);
	}

	RebuildStore loaded;
	loaded.load(file);
	check(!loaded.projectChanged("abc"), "the project hash round-trips");
	check(loaded.projectChanged("other"), "a different project hash is a change");
	check(!loaded.targetChanged(true, "def"), "a target hash round-trips");
	check(loaded.targetChanged(false, "def"), "the two targets keep separate hashes");
	check(loaded.patchedOverlays(true) == std::vector<u32>({ 8, 9, 12 }), "the overlay list round-trips");
	check(loaded.patchedOverlays(false).empty(), "an absent overlay list reads as empty");

	// A record that cannot be read costs one rebuild. Refusing to build over a
	// damaged cache would be the worse failure -- and the binary format this
	// replaces could be misread rather than rejected, because it wrote raw
	// std::time_t values whose width is a property of the compiler.
	write(root / "corrupt.json", "{ not: [valid");
	RebuildStore corrupt;
	corrupt.load(root / "corrupt.json");
	check(corrupt.projectChanged("anything"), "an unreadable record asks for a rebuild");

	RebuildStore missing;
	missing.load(root / "nope.json");
	check(missing.projectChanged("anything"), "a missing record asks for a rebuild");
}

int main()
{
	const fs::path root = fs::temp_directory_path() / "ncp_config_test";
	fs::remove_all(root);
	fs::create_directories(root);

	testNodeReading();
	testNodeErrorsPointAtTheLine();
	testNodeRejectsMissingRequirement();
	testParseErrorIsPositioned();
	testListOps();
	testDefineSet();
	testExpanderIsLazy();
	testExpanderDetectsCycles();
	testExpanderEnvironment();

	testV1Reader(root / "v1");
	testV2Reader(root / "v1");   // reuses the source tree the v1 fixture built
	testV2RejectsTypos(root / "v1");
	testMigrationPreservesTheBuild(root / "migrate");
	testRebuildStore(root / "rebuild");

	fs::remove_all(root);

	if (g_failures == 0)
	{
		std::cout << "config_test: all checks passed\n";
		return 0;
	}
	std::cout << g_failures << " config test(s) failed.\n";
	return 1;
}
