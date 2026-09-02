// Tests for source/config/**.
//
// The configuration layer is the one part of NCPatcher whose mistakes are
// silent: a misread flag does not crash, it produces a ROM that is subtly
// wrong. So the cases here lean toward the things that used to go unnoticed:
// a size limit that quietly became the default, a define that reached one
// language and not another, a variable that could only be read if it was
// declared above its user.
//
// Run via ctest, or directly: ./config_test

#include "../source/config/config_loader.hpp"
#include "../source/config/env_file.hpp"
#include "../source/config/expander.hpp"
#include "../source/config/migrate.hpp"
#include "../source/config/node.hpp"
#include "../source/config/rebuild_store.hpp"
#include "../source/config/target_resolver.hpp"
#include "../source/app/config_dump.hpp"
#include "../source/utils/json.hpp"

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
	// undefined behavior rather than a diagnostic.
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

// --var has to beat the file, or it is not an override. It also has to be taken
// literally rather than expanded again, since its value comes from a shell that
// has already done whatever expanding it meant to do.
static void testExpanderOverrides()
{
	const Document doc("a: x\n", "t.yaml");
	const Node origin = doc.root()["a"];

	Expander expander;
	expander.setVariable("ref", "/from/the/file", origin);
	expander.setOverride("ref", "/from/the/command/line");

	checkEqual(expander.expand("${vars.ref}/include", origin), "/from/the/command/line/include",
		"an override wins over the file's own value");

	expander.setOverride("only", "${vars.ref}");
	checkEqual(expander.expand("${vars.only}", origin), "${vars.ref}",
		"an override is used as written, not expanded again");

	const std::vector<std::string> names = expander.knownNames();
	check(std::count(names.begin(), names.end(), "vars.ref") == 1,
		"a name that is both declared and overridden is listed once");
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

// Environment file =======================================================

static void testEnvFileParsing(const fs::path& root)
{
	write(root / "ok.env",
		"# a comment\n"
		"\n"
		"  SPACED  =  /padded/path  \n"
		"QUOTED=\"/with spaces/\"\n"
		"EMPTY=\n"
		"DUP=first\n"
		"DUP=second\n");

	EnvFile env;
	env.load(root / "ok.env");
	check(env.loaded(), "an existing env file reports loaded");
	checkEqual(*env.find("SPACED"), "/padded/path", "surrounding whitespace is trimmed");
	checkEqual(*env.find("QUOTED"), "/with spaces/", "one layer of quotes is stripped");
	checkEqual(*env.find("EMPTY"), "", "an empty value is allowed");
	checkEqual(*env.find("DUP"), "second", "a repeated name takes the last assignment");
	check(env.find("MISSING") == nullptr, "an absent name is null");

	// A file that is not there is not an error; most projects have none.
	EnvFile absent;
	absent.load(root / "nope.env");
	check(!absent.loaded(), "a missing env file is not loaded");
	check(absent.empty(), "a missing env file is empty");

	write(root / "bad.env", "NAME=ok\nthis is not an assignment\n");
	const std::string malformed = errorFrom([&] { EnvFile bad; bad.load(root / "bad.env"); });
	check(contains(malformed, "bad.env:2"), "a malformed line is reported with its line number");

	write(root / "shell.env", "export NAME=ok\n");
	const std::string exported = errorFrom([&] { EnvFile bad; bad.load(root / "shell.env"); });
	check(contains(exported, "export"), "the export keyword is diagnosed by name");

	write(root / "name.env", "9LIVES=ok\n");
	const std::string badName = errorFrom([&] { EnvFile bad; bad.load(root / "name.env"); });
	check(contains(badName, "Invalid variable name"), "a name that cannot be a variable is an error");
}

static void testEnvFileBeatsTheEnvironment(const fs::path& root)
{
	// The whole point: a value in the project's file wins over one exported by
	// whatever shell happens to be running the build.
	write(root / "pinned.env", "NCP_TEST_REF=/from/the/file\n");
	EnvFile env;
	env.load(root / "pinned.env");

	const Document doc("a: x\n", "t.yaml");
	const Node origin = doc.root()["a"];

	Expander expander;
	expander.setEnvFile(&env);
	checkEqual(expander.expand("${env.NCP_TEST_REF}", origin), "/from/the/file",
		"the env file supplies a variable the environment does not have");

	// And it still wins when the environment does have one.
#ifdef _WIN32
	_putenv_s("NCP_TEST_REF", "/from/the/shell");
#else
	setenv("NCP_TEST_REF", "/from/the/shell", 1);
#endif
	checkEqual(expander.expand("${env.NCP_TEST_REF}", origin), "/from/the/file",
		"the env file overrides the ambient environment");

	Expander plain;
	checkEqual(plain.expand("${env.NCP_TEST_REF}", origin), "/from/the/shell",
		"without the file, the ambient environment is used");
#ifdef _WIN32
	_putenv_s("NCP_TEST_REF", "");
#else
	unsetenv("NCP_TEST_REF");
#endif
}

static void testExpanderDefersNames()
{
	const Document doc("a: x\n", "t.yaml");
	const Node origin = doc.root()["a"];

	Expander expander;
	expander.setDeferred("variant.name");

	// A deferred name expands to itself, so a later pass can finish it. It must
	// not be reported as unknown, and must not consume the surrounding text.
	checkEqual(expander.expand("out/${variant.name}.xdelta", origin), "out/${variant.name}.xdelta",
		"a deferred reference survives expansion verbatim");

	const std::string unknown = errorFrom([&] { (void)expander.expand("${variant.other}", origin); });
	check(contains(unknown, "Unknown reference"), "deferring one name does not defer its neighbors");
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
	"pre-build": ["echo legacy pre"],
	"post-build": ["echo legacy post"],
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
	check(config.hooks.size() == 2, "v1 command arrays become hooks");
	checkEqual(config.hooks[0].name, "pre-build #1", "a legacy hook gets a stable name");
	check(config.hooks[1].when == HookWhen::PostBuild, "the legacy post-build phase survives");

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
	const BuildTarget resolved = TargetResolver::resolve(config, config.arm9, paths, nullptr, quiet);

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
  banner: nitrofs/banner.bin

toolchain: { prefix: arm-none-eabi- }

modules:
  dump: build/generated/modules.json
  enabled: []

hooks:
  - name: Generate module headers
    run: generate ${ncp.moduleDump}
    cwd: ${project.root}/tools
    env:
      MODULE_DUMP: ${ncp.moduleDump}
      HOOK_MODE: ${env.NCP_HOOK_MODE:-portable}
    when: pre-build
  - name: Generate file ids
    run: fid --variant ${variant.name} --manifest ${ncp.fileDump}
    when: post-files
  - name: Report result
    run: report ${rom.output}
    when: post-build

files:
  sp/demo/readme.txt: ${project.root}/nitrofs/readme.txt
  z_new/coop/new.bin: generated/new.bin
  demo/boot_sub_bg_ncg.bin:
    source: nitrofs/demo/boot_sub_bg_ncg.bin
    id: 1209
  ARCHIVE/menu_title.narc!menu/title/USA/vs.bmg: nitrofs/fr/vs.bmg

file-trees:
  - dir: nitrofs
    layered: true
    base-variant: en

files-dump: build/generated/files.json
files-reserve: z_new/reserved

variants:
  en:
    defines: GAME_LANGUAGE_EN
    files:
      sp/demo/readme.txt: nitrofs/en/readme.txt
  fr:
    defines: [GAME_LANGUAGE_FR, MESSAGE_LANGUAGE=2]
    banner: nitrofs/fr/banner.bin
    files:
      sp/demo/readme.txt: nitrofs/fr/readme.txt
      z_new/coop/localized.bin: generated/fr.bin
    module-variants:
      thirdparty: french

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
	check(config.hooks.size() == 3, "structured hooks read back");
	checkEqual(config.hooks[0].name, "Generate module headers", "a hook keeps its name");
	check(config.hooks[0].when == HookWhen::PreBuild, "a hook keeps its phase");
	checkEqual(config.hooks[0].cwd.generic_string(), (root / "tools").generic_string(),
		"hook cwd expands project.root");
	const std::string moduleDump = (root / "build/generated/modules.json").generic_string();
	checkEqual(config.hooks[0].run, "generate " + moduleDump,
		"ncp.moduleDump expands to the absolute dump path");
	check(config.hooks[0].env.size() == 2, "a hook reads environment overrides");
	checkEqual(config.hooks[0].env[0].second, moduleDump,
		"hook environment values expand ncp.moduleDump");
	checkEqual(config.hooks[0].env[1].second, "portable",
		"hook environment values use normal env fallback expansion");
	check(config.hooks[1].when == HookWhen::PostFiles, "post-files is a hook phase");

	// A top-level key rather than `files.dump`: `files:` is a mapping of ROM
	// paths, and a ROM may perfectly well contain a file called `dump`.
	checkEqual(config.filesDump.value.generic_string(), "build/generated/files.json",
		"files-dump reads back");
	const std::string fileDump = (root / "build/generated/files.json").generic_string();
	check(config.hooks[2].when == HookWhen::PostBuild, "post-build is a hook phase");

	// Neither is knowable while the file is being read, so both must survive it
	// intact for the hook runner to finish. A reader that resolved them here
	// would bake the first variant's name into every later variant's command.
	checkEqual(config.hooks[1].run, "fid --variant ${variant.name} --manifest " + fileDump,
		"variant.name is deferred to the hook runner, and ncp.fileDump is not");
	checkEqual(config.hooks[2].run, "report ${rom.output}",
		"rom.output is deferred to the hook runner");
	check(config.files.size() == 4, "NitroFS files read back");
	checkEqual(config.files[0].path, "sp/demo/readme.txt", "a NitroFS destination keeps '/' separators");
	checkEqual(config.files[0].source.generic_string(), (root / "nitrofs/readme.txt").generic_string(),
		"a NitroFS source expands project.root");
	checkEqual(config.files[1].source.generic_string(), "generated/new.bin",
		"a relative NitroFS source stays relative to the project");
	check(config.files[0].id == -1 && config.files[1].id == -1,
		"an ordinary NitroFS entry claims no file id");
	checkEqual(config.files[2].path, "demo/boot_sub_bg_ncg.bin",
		"the mapping form of a NitroFS entry keeps its destination");
	check(config.files[2].id == 1209, "and reads the file id it claims");
	checkEqual(config.files[2].source.generic_string(), "nitrofs/demo/boot_sub_bg_ncg.bin",
		"the mapping form reads its source from a key rather than the value");

	// Two coordinates in one string, separated the way jar: and zip: URIs
	// separate the same two things. There is no directory to carry the tree's
	// `_narc` convention here, so the destination has to say it outright.
	checkEqual(config.files[3].path, "ARCHIVE/menu_title.narc!menu/title/USA/vs.bmg",
		"a destination may name a file inside a Nitro archive");
	const config::NitroDestination inArchive = config::splitNitroDestination(config.files[3].path);
	check(inArchive.inArchive, "and it reads as an archive destination");
	checkEqual(inArchive.path, "ARCHIVE/menu_title.narc", "the archive is the ROM file");
	checkEqual(inArchive.inner, "menu/title/USA/vs.bmg", "and the rest names the member");
	check(!config::splitNitroDestination("uiStudio/title.bin").inArchive,
		"an ordinary path is not an archive destination");

	// Both halves are ordinary NitroFS paths, so both get the ordinary rules,
	// and a second '!' has no reading at all, since nothing here opens an
	// archive inside an archive.
	check(config::nitroDestinationProblem("ARCHIVE/x.narc!a/b.bin").empty(),
		"a well-formed archive destination is accepted");
	check(!config::nitroDestinationProblem("ARCHIVE/x.narc!a!b").empty(),
		"a destination with two '!' is refused");
	check(!config::nitroDestinationProblem("ARCHIVE/x.narc!").empty(),
		"an archive destination naming no member is refused");
	check(!config::nitroDestinationProblem("!a/b.bin").empty(),
		"a member with no archive is refused");
	check(!config::nitroDestinationProblem("ARCHIVE/x.narc!../b.bin").empty(),
		"'..' inside an archive is refused like anywhere else");

	// The placeholder that spends the first new file id on nothing. Off unless
	// a project asks for it: which id a game's compiled code treats as a
	// sentinel is a fact about that game, not about NitroFS.
	checkEqual(config.filesReserve.value, "z_new/reserved", "files-reserve reads back");

	// Not part of `files:`: the banner is a region the header points at rather
	// than a NitroFS file, so no path would name it.
	checkEqual(config.romBanner.value.generic_string(), "nitrofs/banner.bin", "rom.banner reads back");
	checkEqual(config.variants[1].banner.generic_string(), "nitrofs/fr/banner.bin",
		"a variant may override the banner");
	check(config.variants[0].banner.empty(), "and a variant that does not is left alone");

	check(config.fileTrees.size() == 1, "a file tree reads back");
	checkEqual(config.fileTrees[0].dir.generic_string(), "nitrofs", "a file tree keeps its directory");
	check(config.fileTrees[0].layered, "a file tree reads its layered flag");
	checkEqual(config.fileTrees[0].baseVariant, "en", "a file tree reads its base variant");
	check(config.variants.size() == 2, "variants preserve their declaration order");
	checkEqual(config.variants[0].name, "en", "a variant keeps its name");
	check(config.variants[0].defines == std::vector<std::string>({ "GAME_LANGUAGE_EN" }),
		"a scalar variant define becomes a one-entry list");
	checkEqual(config.variants[0].files[0].source.generic_string(), "nitrofs/en/readme.txt",
		"a variant reads its file override");
	check(config.variants[1].defines.size() == 2 && config.variants[1].files.size() == 2,
		"a variant carries its complete define/file matrix");
	check(config.variants[0].moduleVariants.empty(),
		"a variant maps no module layers unless it says so");
	check(config.variants[1].moduleVariants.size() == 1
		&& config.variants[1].moduleVariants[0].first == "thirdparty"
		&& config.variants[1].moduleVariants[0].second == "french",
		"a variant reads the module layer names it maps itself onto");

	PathContext paths;
	paths.workDir = root;
	paths.targetWorkDir = root / "code";
	TargetResolver::Options quiet;
	quiet.quiet = true;
	const BuildTarget resolved = TargetResolver::resolve(config, config.arm9, paths, nullptr, quiet);

	check(resolved.arenaLo == 0x02065F10, "arena-lo reads as an address");

	// Inheritance: project common, then target common, then the language list,
	// then the defines, which reach assembly too.
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

// What `config dump --json` has to carry for a consumer to resolve the same
// module trees the build does. Big Star Editor hand-parsed ncpatcher.yaml for
// these three because the dump omitted them, which is precisely how a second
// implementation of the resolution rules drifts out of step with this one.
static void testV2DumpCarriesResolutionInputs(const fs::path& root)
{
	const ProjectConfig config = loadV2(root / "ncpatcher.yaml", root);

	PathContext paths;
	paths.workDir = root;
	paths.romDir = root / "__tmp";
	DumpOptions options;
	options.json = true;

	std::ostringstream out;
	dumpConfig(out, config, paths, {}, options);
	const std::string json = out.str();

	const std::string banner = Json::escape((root / "nitrofs" / "banner.bin").string());
	const std::string frBanner = Json::escape((root / "nitrofs" / "fr" / "banner.bin").string());

	// Resolved against the work directory like every other ROM path, so a
	// consumer never has to know what a project-relative path is relative to.
	check(contains(json, "\"rom-banner\": \"" + banner + "\""),
		"the project banner is dumped as a resolved path");
	check(contains(json, "\"banner\": \"" + frBanner + "\""),
		"a variant's banner override is dumped as well");
	check(contains(json, "\"thirdparty\": \"french\""),
		"module-variants names the layer a variant selects in a module's own tree");

	// Unconditional keys: a consumer should not have to test for a key's
	// existence to learn that a variant overrides nothing.
	check(contains(json, "\"banner\": \"\""),
		"a variant with no banner still gets the key, empty");
	check(contains(json, "\"module-variants\": {}"),
		"and a variant mapping no module layers gets an empty object");

	// The human form answers the same question for whoever is reading it.
	std::ostringstream human;
	DumpOptions plain;
	dumpConfig(human, config, paths, {}, plain);
	check(contains(human.str(), "module variant: thirdparty -> french"),
		"the human dump reports the mapping too");
}

// Most of a DS game is assets, so a project that only replaces some of them is
// an ordinary project and not a degenerate one. It compiles nothing, needs no
// toolchain, and has no ARM binary to name.
static void testV2AllowsAProjectWithNoCode(const fs::path& root)
{
	write(root / "assets-only.yaml", R"(version: 2

rom:
  file: game.nds
  output: build/game.nds
  backup: backup

file-trees:
  - dir: nitrofs
)");

	const ProjectConfig config = loadV2(root / "assets-only.yaml", root);
	check(!config.arm7.enabled && !config.arm9.enabled, "no targets are enabled");
	check(config.arm7.name == "arm7" && config.arm9.name == "arm9",
		"both targets are still named, so anything iterating them still works");
	check(config.fileTrees.size() == 1, "the tree that does the work is read");

	// Each of these is the whole reason a build would run.
	for (const char* work : { "files:\n  a/b.bin: assets/b.bin\n",
	                          "modules:\n  dir: modules\n",
	                          "files-reserve: z_new/reserved\n",
	                          "hooks:\n  - name: Generate\n    run: echo hi\n    when: pre-build\n" })
	{
		write(root / "one-job.yaml", std::string(R"(version: 2

rom:
  file: game.nds
  backup: backup

)") + work);
		const ProjectConfig one = loadV2(root / "one-job.yaml", root);
		check(!one.arm7.enabled && !one.arm9.enabled, std::string("no code, but ") + work + "is work");
	}

	// Nothing to compile is a project. Nothing at all is a mistake, and saying
	// so beats reporting success for having copied a ROM.
	write(root / "empty.yaml", R"(version: 2

rom:
  file: game.nds
  backup: backup
)");
	const std::string error = errorFrom([&] { (void)loadV2(root / "empty.yaml", root); });
	check(contains(error, "nothing to build"), "a project that does nothing is refused");
	check(contains(error, "file-trees"), "and is told what it could have said instead");
}

static void testV2RejectsTypos(const fs::path& root)
{
	const std::string bad = std::string(V2_PROJECT).replace(
		std::string(V2_PROJECT).find("maxsize"), 7, "maxsze");
	write(root / "typo.yaml", bad);

	const std::string error = errorFrom([&] { (void)loadV2(root / "typo.yaml", root); });
	check(contains(error, "maxsze"), "an unknown key is named");
	check(contains(error, "Expected one of"), "an unknown key lists what was expected");

	std::string badVariant = V2_PROJECT;
	const std::string variantDefine = "defines: GAME_LANGUAGE_EN";
	badVariant.replace(badVariant.find(variantDefine), variantDefine.size(),
		variantDefine + "\n    output: build/en.nds");
	write(root / "bad-variant-key.yaml", badVariant);
	const std::string variantKey = errorFrom([&] { (void)loadV2(root / "bad-variant-key.yaml", root); });
	check(contains(variantKey, "output"), "an unknown variant key is rejected");

	std::string badVariantName = V2_PROJECT;
	badVariantName.replace(badVariantName.find("  en:\n"), 6, "  ../en:\n");
	write(root / "bad-variant-name.yaml", badVariantName);
	const std::string variantName = errorFrom([&] { (void)loadV2(root / "bad-variant-name.yaml", root); });
	check(contains(variantName, "variant name"), "a variant name cannot become a path");

	std::string duplicateVariant = V2_PROJECT;
	duplicateVariant.insert(duplicateVariant.find("\ndefines: [SDK_GCC]"),
		"  EN:\n    defines: DUPLICATE\n");
	write(root / "duplicate-variant.yaml", duplicateVariant);
	const std::string duplicate = errorFrom([&] { (void)loadV2(root / "duplicate-variant.yaml", root); });
	check(contains(duplicate, "more than once"), "variant names cannot collide by case");

	std::string badWhen = V2_PROJECT;
	const std::string phaseText = "when: pre-build";
	badWhen.replace(badWhen.find(phaseText), phaseText.size(), "when: during");
	write(root / "bad-hook-phase.yaml", badWhen);
	const std::string phase = errorFrom([&] { (void)loadV2(root / "bad-hook-phase.yaml", root); });
	check(contains(phase, "Invalid hook phase"), "an unknown hook phase is rejected");

	write(root / "mixed-hooks.yaml", std::string(V2_PROJECT) + "\npre-build: echo old\n");
	const std::string mixed = errorFrom([&] { (void)loadV2(root / "mixed-hooks.yaml", root); });
	check(contains(mixed, "not both"), "structured and compatibility hooks cannot be mixed");

	// Two entries claiming one id is the failure the Python accepts silently:
	// whichever ran second would rename a file the first had already renamed.
	std::string duplicateId = V2_PROJECT;
	duplicateId.insert(duplicateId.find("\nfile-trees:"),
		"  demo/UE_title_nsc.bin:\n    source: nitrofs/demo/UE_title_nsc.bin\n    id: 1209\n");
	write(root / "duplicate-id.yaml", duplicateId);
	const std::string sharedId = errorFrom([&] { (void)loadV2(root / "duplicate-id.yaml", root); });
	check(contains(sharedId, "1209") && contains(sharedId, "claimed by both"),
		"two NitroFS entries cannot claim one file id");

	std::string badId = V2_PROJECT;
	badId.replace(badId.find("id: 1209"), 8, "id: 70000");
	write(root / "bad-id.yaml", badId);
	const std::string outOfRange = errorFrom([&] { (void)loadV2(root / "bad-id.yaml", root); });
	check(contains(outOfRange, "65535"), "a file id outside the FAT's range is rejected");

	std::string badTreeKey = V2_PROJECT;
	badTreeKey.replace(badTreeKey.find("    layered: true"), 17, "    layerd: true");
	write(root / "bad-tree-key.yaml", badTreeKey);
	const std::string treeKey = errorFrom([&] { (void)loadV2(root / "bad-tree-key.yaml", root); });
	check(contains(treeKey, "layerd"), "an unknown file tree key is named");

	// base-variant only means anything under layering, so accepting it without
	// would silently do nothing.
	std::string strayBase = V2_PROJECT;
	strayBase.replace(strayBase.find("    layered: true\n"), 18, "");
	write(root / "stray-base.yaml", strayBase);
	const std::string base = errorFrom([&] { (void)loadV2(root / "stray-base.yaml", root); });
	check(contains(base, "base-variant"), "a base variant without layering is rejected");

	std::string emptyLayer = V2_PROJECT;
	emptyLayer.replace(emptyLayer.find("thirdparty: french"), 18, "thirdparty: \"\"");
	write(root / "empty-layer.yaml", emptyLayer);
	const std::string layer = errorFrom([&] { (void)loadV2(root / "empty-layer.yaml", root); });
	check(contains(layer, "cannot be empty"), "a mapped module variant name cannot be empty");

	std::string unsafeFile = V2_PROJECT;
	const std::string safePath = "sp/demo/readme.txt";
	unsafeFile.replace(unsafeFile.find(safePath), safePath.size(), "../outside.bin");
	write(root / "unsafe-file.yaml", unsafeFile);
	const std::string unsafe = errorFrom([&] { (void)loadV2(root / "unsafe-file.yaml", root); });
	check(contains(unsafe, "cannot contain"), "a NitroFS path cannot escape through '..'");
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
	const BuildTarget resolvedBefore = TargetResolver::resolve(before, before.arm9, paths, nullptr, quiet);
	const BuildTarget resolvedAfter = TargetResolver::resolve(after, after.arm9, paths, nullptr, quiet);

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
	// damaged cache would be the worse failure, and the binary format this
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
	testExpanderOverrides();
	testExpanderEnvironment();
	testExpanderDefersNames();
	testEnvFileParsing(root / "env");
	testEnvFileBeatsTheEnvironment(root / "env");

	testV1Reader(root / "v1");
	testV2Reader(root / "v1");   // reuses the source tree the v1 fixture built
	testV2AllowsAProjectWithNoCode(root / "v1");
	testV2RejectsTypos(root / "v1");
	testV2DumpCarriesResolutionInputs(root / "v1");
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
