// Tests for source/modules/**.
//
// The module system's failure modes are the ones the Python prototype had and
// could not see: a component name written twice, two modules claiming the same
// source file, a project retargeting something the module said it must not.
// None of those crash -- they produce a build that is quietly not the one
// anybody asked for -- so most of what is checked here is that they are
// reported at all, and that the report names both halves of the problem.
//
// Run via ctest, or directly: ./modules_test

#include "../source/config/config_loader.hpp"
#include "../source/config/target_resolver.hpp"
#include "../source/modules/module_resolver.hpp"
#include "../source/modules/module_dump.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using namespace ncp;
using namespace ncp::modules;

static int g_failures = 0;

static void check(bool condition, const std::string& what)
{
	if (!condition)
	{
		std::cout << "FAIL: " << what << "\n";
		g_failures++;
	}
}

static bool contains(const std::string& haystack, const std::string& needle)
{
	return haystack.find(needle) != std::string::npos;
}

static void write(const fs::path& path, const std::string& text)
{
	fs::create_directories(path.parent_path());
	std::ofstream out(path, std::ios::binary);
	out << text;
}

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

// Builds a project tree with two modules and returns its root.
//
// `alpha` sweeps its whole source directory into the main binary and pulls two
// files out of it by component, which is the shape every real module has: a
// catch-all plus the exceptions.
static fs::path makeProject(const fs::path& root)
{
	fs::remove_all(root);

	write(root / "modules" / "alpha" / "module.yaml", R"YAML(id: Alpha
name: Alpha
description: The first one.
authors: [someone]
defines: [ALPHA_ON]
targets:
  - arm9:
      includes: "include"
      sources: "source/**"
components:
  - Scene:
      target: arm9(9)
      sources: "source/scene.cpp"
      defines: ALPHA_SCENE=1
      files: ["data/scene.bin"]
      objects:
        - name: SceneObject
          type: scene
  - Extra:
      target: "!arm9(12)"
      sources: "source/extra.cpp"
  - Flagged:
      defines: ALPHA_FLAG
)YAML");
	write(root / "modules" / "alpha" / "include" / "alpha.h", "\n");
	write(root / "modules" / "alpha" / "source" / "main.cpp", "\n");
	write(root / "modules" / "alpha" / "source" / "scene.cpp", "\n");
	write(root / "modules" / "alpha" / "source" / "extra.cpp", "\n");

	write(root / "modules" / "beta" / "module.yaml", R"YAML(id: Beta
targets:
  - arm7:
      sources: "source7/*"
components:
  - Needy:
      target: arm7
      requires: Alpha.Scene
)YAML");
	write(root / "modules" / "beta" / "source7" / "b.cpp", "\n");

	return root;
}

static const char* PROJECT_HEAD = R"YAML(version: 2
rom:
  dir: rom
  backup: backup
targets:
  arm9:
    build: build/arm9
    regions:
      - dest: main
        sources: []
  arm7:
    build: build/arm7
    regions:
      - dest: main
        sources: []
)YAML";

static config::ProjectConfig load(const fs::path& root, const std::string& modulesSection)
{
	write(root / "ncpatcher.yaml", std::string(PROJECT_HEAD) + modulesSection);
	return config::load(root / "ncpatcher.yaml", root, {});
}

static ModuleGraph resolveQuiet(const config::ProjectConfig& config, const fs::path& root)
{
	ResolveOptions options;
	options.quiet = true;
	return resolve(config.modules, root, options);
}

// The happy path ---------------------------------------------------------

static void testResolvesTheGraph(const fs::path& root)
{
	makeProject(root);
	const config::ProjectConfig config = load(root, R"YAML(modules:
  enabled: [alpha, beta]
)YAML");
	const ModuleGraph graph = resolveQuiet(config, root);

	check(graph.modules().size() == 2, "both modules are in the graph");

	const ResolvedModule* alpha = graph.find("Alpha");
	check(alpha != nullptr, "a module is findable by id as well as by key");
	check(alpha != nullptr && alpha->key == "alpha", "the id and the key both reach the same module");

	const TargetContribution& arm9 = graph.contribution(true);

	// MODULE_<ID> and the module's own defines land on the processor it targets.
	const auto hasDefine = [](const TargetContribution& c, const std::string& name) {
		return std::any_of(c.defines.begin(), c.defines.end(),
			[&](const ResolvedDefine& d) { return d.name == name; });
	};
	check(hasDefine(arm9, "MODULE_ALPHA"), "MODULE_<ID> is defined for the processor the module targets");
	check(hasDefine(arm9, "ALPHA_ON"), "a module-level define reaches the target");
	check(hasDefine(arm9, "ALPHA_SCENE"), "a component define reaches the target");
	check(hasDefine(arm9, "ALPHA_FLAG"), "a component with no target still contributes its defines");
	check(!hasDefine(graph.contribution(false), "MODULE_ALPHA"),
		"a module that targets only arm9 does not define itself for arm7");
	check(hasDefine(graph.contribution(false), "MODULE_BETA"), "arm7 gets its own module's define");

	check(arm9.includes.size() == 1, "the module's include directory is contributed once");

	// The catch-all sweeps source/**, but the two files a component claimed go
	// where the component said instead. This is the behaviour the whole design
	// turns on: a module names a directory, and its components carve exceptions
	// out of it.
	const auto names = [](const std::vector<fs::path>& paths) {
		std::vector<std::string> out;
		for (const fs::path& path : paths)
			out.push_back(path.filename().string());
		std::sort(out.begin(), out.end());
		return out;
	};

	check(names(arm9.regionSources.at(-1)) == std::vector<std::string>({ "main.cpp" }),
		"the catch-all keeps only what no component claimed");
	check(names(arm9.regionSources.at(9)) == std::vector<std::string>({ "scene.cpp" }),
		"a component's source goes to the component's overlay");
	check(names(arm9.regionSources.at(12)) == std::vector<std::string>({ "extra.cpp" }),
		"a locked component's source goes where the module locked it");
}

static void testFoldsIntoTheTarget(const fs::path& root)
{
	makeProject(root);
	const config::ProjectConfig config = load(root, R"YAML(modules:
  auto-create-regions: true
  enabled: [alpha, beta]
)YAML");
	const ModuleGraph graph = resolveQuiet(config, root);

	PathContext paths;
	paths.workDir = root;
	paths.targetWorkDir = root;
	config::TargetResolver::Options quiet;
	quiet.quiet = true;

	const BuildTarget target =
		config::TargetResolver::resolve(config, config.arm9, paths, &graph, quiet);

	check(target.regions.size() == 3, "the overlays a module targeted became regions");
	check(target.getRegionByDestination(9) != nullptr, "an auto-created region carries its overlay id");
	check(contains(target.cppFlags, "-DMODULE_ALPHA"), "module defines reach the compiler flags");

	// Absolute in the graph, relative in the target: object files are laid out
	// by source path, and an absolute one lands in build/external/<mangled>.
	const BuildTarget::Region* main = target.getMainRegion();
	check(main != nullptr && !main->sources.empty(), "the main region has the module's sources");
	check(main != nullptr && !main->sources.front().is_absolute(),
		"a module source inside the project is made relative again");
}

// A region that only ever appends and never received anything is noise the
// module system is meant to delete -- but one that reserves space or blanks
// code means something with no sources at all.
static void testPrunesOnlyEmptyAppendRegions(const fs::path& root)
{
	makeProject(root);
	write(root / "ncpatcher.yaml", R"YAML(version: 2
rom:
  dir: rom
  backup: backup
modules:
  enabled: [alpha]
targets:
  arm9:
    build: build/arm9
    regions:
      - dest: main
        sources: []
      - dest: ov9
        sources: []
      - dest: ov12
        sources: []
      - dest: ov30
        sources: []
      - dest: ov31
        sources: []
        overwrites: [[0x02000000, 0x02000100]]
      - dest: ov32
        mode: replace
        sources: []
)YAML");
	const config::ProjectConfig config = config::load(root / "ncpatcher.yaml", root, {});
	const ModuleGraph graph = resolveQuiet(config, root);

	PathContext paths;
	paths.workDir = root;
	paths.targetWorkDir = root;
	config::TargetResolver::Options quiet;
	quiet.quiet = true;

	const BuildTarget target =
		config::TargetResolver::resolve(config, config.arm9, paths, &graph, quiet);

	check(target.getRegionByDestination(30) == nullptr, "an empty append region is dropped");
	check(target.getRegionByDestination(31) != nullptr, "an overwrites region survives having no sources");
	check(target.getRegionByDestination(32) != nullptr, "a replace region survives having no sources");
	check(target.getRegionByDestination(9) != nullptr, "a declared region a module filled is kept");
}

// Project overrides ------------------------------------------------------

static void testProjectOverrides(const fs::path& root)
{
	makeProject(root);
	const config::ProjectConfig config = load(root, R"YAML(modules:
  auto-create-regions: true
  enabled:
    - alpha:
        components:
          Scene: { target: arm9, defines: { ALPHA_SCENE: 7 } }
          Flagged: false
    - beta: false
)YAML");
	const ModuleGraph graph = resolveQuiet(config, root);

	const ResolvedModule* alpha = graph.find("alpha");
	check(alpha != nullptr, "the module resolved");

	const ResolvedComponent* scene = alpha->findComponent("Scene");
	check(scene != nullptr && scene->target.overlay == -1, "the project retargeted the component");
	check(scene != nullptr && scene->targetOrigin == TargetOrigin::Project,
		"the graph remembers that the project chose the target");

	const ResolvedComponent* flagged = alpha->findComponent("Flagged");
	check(flagged != nullptr && !flagged->enabled, "a component can be switched off");
	check(flagged != nullptr && !flagged->disabledReason.empty(), "a disabled component says why");

	const auto& defines = graph.contribution(true).defines;
	const auto it = std::find_if(defines.begin(), defines.end(),
		[](const ResolvedDefine& d) { return d.name == "ALPHA_SCENE"; });
	check(it != defines.end() && it->value == "7", "a project override replaces the define's value");
	check(std::none_of(defines.begin(), defines.end(),
		[](const ResolvedDefine& d) { return d.name == "ALPHA_FLAG"; }),
		"a disabled component contributes no defines");

	// The disabled module was never read, so nothing of it reaches the build --
	// but it is still listed, because "why did this contribute nothing" is a
	// question worth being able to answer.
	const ResolvedModule* beta = graph.find("beta");
	check(beta != nullptr && beta->def == nullptr, "a disabled module is listed but not read");
	check(std::none_of(graph.contribution(false).defines.begin(),
		graph.contribution(false).defines.end(),
		[](const ResolvedDefine& d) { return d.name == "MODULE_BETA"; }),
		"a disabled module defines nothing");
}

// A locked target is the module saying the choice is not the project's to make.
// The prototype swallowed the override; saying so is the whole fix.
static void testLockedTargetRefusesOverride(const fs::path& root)
{
	makeProject(root);
	const config::ProjectConfig config = load(root, R"YAML(modules:
  auto-create-regions: true
  enabled:
    - alpha:
        components:
          Extra: { target: arm9(40) }
)YAML");
	const ModuleGraph graph = resolveQuiet(config, root);

	const ResolvedComponent* extra = graph.find("alpha")->findComponent("Extra");
	check(extra != nullptr && extra->target.overlay == 12, "a locked target is not moved");
	check(extra != nullptr && extra->targetOrigin == TargetOrigin::OverrideRefused,
		"the refusal is recorded rather than silently applied");
}

// Errors -----------------------------------------------------------------

static void testDuplicateComponentIsAnError(const fs::path& root)
{
	makeProject(root);
	write(root / "modules" / "alpha" / "module.yaml", R"YAML(id: Alpha
targets:
  - arm9:
      sources: "source/**"
components:
  - Twice:
      target: arm9
      sources: "source/scene.cpp"
  - Twice:
      target: arm9
      sources: "source/extra.cpp"
)YAML");
	const config::ProjectConfig config = load(root, "modules:\n  enabled: [alpha]\n");
	const std::string error = errorFrom([&] { (void)resolveQuiet(config, root); });

	check(contains(error, "Twice"), "the duplicate component is named");
	check(contains(error, "more than once"), "the duplicate is reported as one");
	check(contains(error, "module.yaml:"), "the report points at the file");
}

static void testTwoClaimantsIsAnError(const fs::path& root)
{
	makeProject(root);
	write(root / "modules" / "beta" / "module.yaml", R"YAML(id: Beta
targets:
  - arm9:
      sources: "../alpha/source/*"
components:
  - Thief:
      target: arm9(20)
      sources: "../alpha/source/scene.cpp"
)YAML");
	const config::ProjectConfig config = load(root, "modules:\n  enabled: [alpha, beta]\n");
	const std::string error = errorFrom([&] { (void)resolveQuiet(config, root); });

	check(contains(error, "scene.cpp"), "the contested file is named");
	check(contains(error, "Alpha.Scene"), "the first claimant is named");
	check(contains(error, "Beta.Thief"), "the second claimant is named");
	check(contains(error, "ov9") && contains(error, "ov20"), "both destinations are named");
}

static void testMissingModuleIsAnError(const fs::path& root)
{
	makeProject(root);
	const config::ProjectConfig config = load(root, "modules:\n  enabled: [alpha, ghost]\n");
	const std::string error = errorFrom([&] { (void)resolveQuiet(config, root); });

	check(contains(error, "ghost"), "the missing module is named");
	check(contains(error, "optional: true"), "the way to say it may be absent is offered");
}

static void testOptionalModuleIsSkipped(const fs::path& root)
{
	makeProject(root);
	const config::ProjectConfig config = load(root, R"YAML(modules:
  enabled:
    - alpha
    - ghost: { optional: true }
)YAML");
	const ModuleGraph graph = resolveQuiet(config, root);
	check(graph.modules().size() == 1, "an optional module that is not installed is skipped");
}

static void testUnknownComponentOverrideIsAnError(const fs::path& root)
{
	makeProject(root);
	const config::ProjectConfig config = load(root, R"YAML(modules:
  enabled:
    - alpha:
        components:
          Nope: false
)YAML");
	const std::string error = errorFrom([&] { (void)resolveQuiet(config, root); });
	check(contains(error, "Nope"), "an override of a component that does not exist is reported");
}

static void testDefineOverrideMustMatchADeclaredDefine(const fs::path& root)
{
	makeProject(root);
	const config::ProjectConfig config = load(root, R"YAML(modules:
  enabled:
    - alpha:
        components:
          Scene: { defines: { INVENTED: 1 } }
)YAML");
	const std::string error = errorFrom([&] { (void)resolveQuiet(config, root); });
	check(contains(error, "INVENTED"), "overriding a define the component never had is reported");
}

static void testDisabledRequirementIsAnError(const fs::path& root)
{
	makeProject(root);
	const config::ProjectConfig config = load(root, R"YAML(modules:
  enabled:
    - alpha:
        components:
          Scene: false
    - beta
)YAML");
	const std::string error = errorFrom([&] { (void)resolveQuiet(config, root); });
	check(contains(error, "Beta.Needy"), "the component with the unmet requirement is named");
	check(contains(error, "Alpha.Scene"), "the requirement is named");
	check(contains(error, "disabled"), "the reason is that it is disabled");
}

static void testInvalidIdIsAnError(const fs::path& root)
{
	makeProject(root);
	write(root / "modules" / "alpha" / "module.yaml", "id: 9Lives\ntargets:\n  - arm9:\n      sources: \"source/*\"\n");
	const config::ProjectConfig config = load(root, "modules:\n  enabled: [alpha]\n");
	const std::string error = errorFrom([&] { (void)resolveQuiet(config, root); });
	check(contains(error, "9Lives"), "an id that cannot become a define name is reported");
}

// Silently inventing the region is how a mistyped overlay id turns into an
// overlay full of code the game never loads.
static void testUndeclaredRegionIsAnError(const fs::path& root)
{
	makeProject(root);
	const config::ProjectConfig config = load(root, "modules:\n  enabled: [alpha]\n");
	const ModuleGraph graph = resolveQuiet(config, root);

	PathContext paths;
	paths.workDir = root;
	paths.targetWorkDir = root;
	config::TargetResolver::Options quiet;
	quiet.quiet = true;

	const std::string error = errorFrom([&] {
		(void)config::TargetResolver::resolve(config, config.arm9, paths, &graph, quiet);
	});
	check(contains(error, "ov9"), "the overlay nobody declared is named");
	check(contains(error, "auto-create-regions"), "the way to allow it is offered");
}

// The dump ---------------------------------------------------------------

static void testDumpCarriesUnknownKeys(const fs::path& root)
{
	makeProject(root);
	const config::ProjectConfig config = load(root, R"YAML(modules:
  auto-create-regions: true
  enabled: [alpha, beta]
)YAML");
	const ModuleGraph graph = resolveQuiet(config, root);

	std::ostringstream out;
	writeDump(out, graph);
	const std::string text = out.str();

	check(contains(text, "\"schema\": \"ncpatcher.modules/1\""), "the dump names its schema");

	// The whole boundary in one assertion: NCPatcher has no idea what an
	// "object" is, and the generator downstream cannot work without them.
	check(contains(text, "SceneObject"), "an unknown component key survives into the dump");
	check(contains(text, "\"extra\""), "unknown keys are grouped under extra");

	check(contains(text, "data/scene.bin"), "a component's files are emitted untouched");
	check(contains(text, "\"requires\""), "requirements are emitted");
	check(contains(text, "\"line\""), "each component carries the line it was declared on");
}

int main()
{
	const fs::path root = fs::temp_directory_path() / "ncp_modules_test";
	fs::remove_all(root);

	testResolvesTheGraph(root / "graph");
	testFoldsIntoTheTarget(root / "fold");
	testPrunesOnlyEmptyAppendRegions(root / "prune");
	testProjectOverrides(root / "override");
	testLockedTargetRefusesOverride(root / "locked");
	testDuplicateComponentIsAnError(root / "dup");
	testTwoClaimantsIsAnError(root / "claim");
	testMissingModuleIsAnError(root / "missing");
	testOptionalModuleIsSkipped(root / "optional");
	testUnknownComponentOverrideIsAnError(root / "unknown");
	testDefineOverrideMustMatchADeclaredDefine(root / "define");
	testDisabledRequirementIsAnError(root / "requires");
	testInvalidIdIsAnError(root / "id");
	testUndeclaredRegionIsAnError(root / "undeclared");
	testDumpCarriesUnknownKeys(root / "dump");

	fs::remove_all(root);

	if (g_failures == 0)
	{
		std::cout << "modules_test: all checks passed\n";
		return 0;
	}
	std::cout << g_failures << " module test(s) failed.\n";
	return 1;
}
