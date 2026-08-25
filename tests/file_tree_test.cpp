// Tests for source/rom/file_tree.cpp -- the resolution order that decides which
// module's copy of a path ends up in the ROM.
//
// The cases here are the ones a real project actually hit, because the rules
// only look obvious until two modules ship the same filename: a module that
// replaces artwork wholesale must beat one that only translates the stock
// version, and a module translating one language must beat one supplying every
// language. Those two pull in opposite directions, so both are pinned.

#include "../source/rom/file_tree.hpp"

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace ncp;
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

static void write(const fs::path& file, std::string_view text)
{
	fs::create_directories(file.parent_path());
	std::ofstream out(file, std::ios::binary);
	out.write(text.data(), std::streamsize(text.size()));
}

// The source a destination resolved to, relative to the fixture root, or "" if
// the sweep did not produce that destination at all.
static std::string sourceOf(const std::vector<config::FileConfig>& files,
                            const fs::path& root, std::string_view path)
{
	for (const config::FileConfig& file : files)
	{
		if (file.path == path)
			return fs::relative(file.source, root).generic_string();
	}
	return {};
}

static const config::FileConfig* entry(const std::vector<config::FileConfig>& files,
                                       std::string_view path)
{
	for (const config::FileConfig& file : files)
	{
		if (file.path == path)
			return &file;
	}
	return nullptr;
}

// A fixture shaped like nsmb-coop-module: one module replacing artwork for
// every language, one translating a subset.
static fs::path buildFixture()
{
	const fs::path root = fs::temp_directory_path() / "ncp_file_tree_test";
	fs::remove_all(root);

	// coop ships its own title graphic in all three languages, and one sign in
	// English only.
	for (const char* lang : { "en", "fr", "de" })
		write(root / "coop" / "nitrofs" / lang / "uiStudio" / "title.bin", std::string("coop-") + lang);
	write(root / "coop" / "nitrofs" / "en" / "enemy" / "sign.nsbmd", "coop-sign");
	write(root / "coop" / "nitrofs" / "en" / "z_new" / "coop" / "sound.nwav", "coop-sound");

	// message translates the stock title graphic and the stock sign, but has
	// nothing in English.
	for (const char* lang : { "fr", "de" })
	{
		write(root / "message" / "nitrofs" / lang / "uiStudio" / "title.bin", std::string("message-") + lang);
		write(root / "message" / "nitrofs" / lang / "enemy" / "sign.nsbmd", std::string("message-") + lang);
	}

	return root;
}

static std::vector<rom::FileTree> fixtureTrees(const fs::path& root)
{
	std::vector<rom::FileTree> trees;
	for (const char* name : { "coop", "message" })
	{
		rom::FileTree tree;
		tree.dir = root / name / "nitrofs";
		tree.layered = true;
		tree.baseVariant = "en";
		tree.module = name;
		tree.origin = std::string("module ") + name;
		trees.push_back(std::move(tree));
	}
	return trees;
}

// --- layering --------------------------------------------------------------

static void testBaseVariantFillsTheGaps()
{
	const fs::path root = buildFixture();
	const auto files = rom::sweepFileTrees(fixtureTrees(root), "fr");

	// English is the base, so a path only English supplies still lands.
	check(sourceOf(files, root, "z_new/coop/sound.nwav") == "coop/nitrofs/en/z_new/coop/sound.nwav",
		"the base variant supplies what the built variant does not");

	const config::FileConfig* sound = entry(files, "z_new/coop/sound.nwav");
	check(sound != nullptr && sound->fromVariant == "en",
		"and the entry records which layer it came from");
}

static void testVariantOverridesItsOwnBase()
{
	const fs::path root = buildFixture();
	const auto files = rom::sweepFileTrees(fixtureTrees(root), "de");
	check(sourceOf(files, root, "uiStudio/title.bin") == "coop/nitrofs/de/uiStudio/title.bin",
		"a variant layer overrides the base layer of the same tree");
}

static void testAVariantMaySupplyWhatNoBaseDid()
{
	const fs::path root = buildFixture();

	// message has no en/ at all, so its French entries have no base underneath.
	// That is ordinary rather than a missing base.
	rom::FileTree tree;
	tree.dir = root / "message" / "nitrofs";
	tree.layered = true;
	tree.baseVariant = "en";
	tree.module = "message";
	tree.origin = "module message";

	const auto files = rom::sweepFileTrees({ tree }, "fr");
	check(sourceOf(files, root, "enemy/sign.nsbmd") == "message/nitrofs/fr/enemy/sign.nsbmd",
		"a variant may supply a path the base never had");
}

static void testAnUntranslatedVariantIsNotAnError()
{
	const fs::path root = buildFixture();
	const auto files = rom::sweepFileTrees(fixtureTrees(root), "ja");
	check(sourceOf(files, root, "uiStudio/title.bin") == "coop/nitrofs/en/uiStudio/title.bin",
		"a variant no tree has a directory for falls back to the base");
}

// --- precedence between modules --------------------------------------------

static void testEarlierModuleWinsWithinOneLayer()
{
	const fs::path root = buildFixture();
	const auto files = rom::sweepFileTrees(fixtureTrees(root), "fr");

	// Both modules ship fr/uiStudio/title.bin. coop is listed first, and that
	// is the project stating which of the two owns the path -- replacing the
	// graphic wholesale has to beat translating the stock one.
	check(sourceOf(files, root, "uiStudio/title.bin") == "coop/nitrofs/fr/uiStudio/title.bin",
		"within one layer the earlier module keeps the destination");

	const config::FileConfig* title = entry(files, "uiStudio/title.bin");
	check(title != nullptr && title->module == "coop", "and the entry is attributed to it");
}

static void testLayerOutranksModuleOrder()
{
	const fs::path root = buildFixture();
	const auto files = rom::sweepFileTrees(fixtureTrees(root), "fr");

	// coop supplies the sign in English only; message translates it. The
	// translation is the more specific claim, so it wins even though coop is
	// listed first -- the opposite outcome to the case above, from the same
	// pair of modules.
	check(sourceOf(files, root, "enemy/sign.nsbmd") == "message/nitrofs/fr/enemy/sign.nsbmd",
		"a variant layer beats an earlier module's base layer");
}

// --- mapping a project variant onto a module's own layer names -------------

static void testAModuleThatNamesItsLayersDifferently()
{
	const fs::path root = buildFixture();

	// A module written for a different project: its tree is split by the words
	// it chose, not by the language codes this project uses.
	write(root / "thirdparty" / "nitrofs" / "french" / "uiStudio" / "title.bin", "thirdparty-french");

	rom::FileTree tree;
	tree.dir = root / "thirdparty" / "nitrofs";
	tree.layered = true;
	tree.module = "thirdparty";
	tree.origin = "module thirdparty";

	// Unmapped, the two never meet, and the module silently contributes nothing.
	check(rom::sweepFileTrees({ tree }, "fr").empty(),
		"a module whose layers are named differently contributes nothing on its own");

	tree.variantLayer = "french";
	const auto files = rom::sweepFileTrees({ tree }, "fr");
	check(sourceOf(files, root, "uiStudio/title.bin") == "thirdparty/nitrofs/french/uiStudio/title.bin",
		"mapping the variant onto the module's own layer name connects the two");
}

static void testAMappedLayerMustExist()
{
	const fs::path root = buildFixture();

	rom::FileTree tree;
	tree.dir = root / "coop" / "nitrofs";
	tree.layered = true;
	tree.baseVariant = "en";
	tree.module = "coop";
	tree.origin = "module coop";

	// Unmapped, a missing layer is ordinary and falls back to the base.
	check(!rom::sweepFileTrees({ tree }, "ja").empty(),
		"an unmapped variant with no directory falls back to the base");

	// Mapped, it is an assertion the project made, so a typo has to be caught
	// rather than honoured by quietly shipping the base layer instead.
	tree.variantLayer = "franch";
	bool threw = false;
	try { (void)rom::sweepFileTrees({ tree }, "fr"); }
	catch (const std::exception&) { threw = true; }
	check(threw, "a layer the project named explicitly must exist");
}

static void testMappingDoesNotDisturbTheBaseLayer()
{
	const fs::path root = buildFixture();

	// The module's base is its own declaration, independent of what the
	// project's variants are called, so mapping the variant layer leaves it be.
	write(root / "coop" / "nitrofs" / "french" / "uiStudio" / "title.bin", "coop-french");

	rom::FileTree tree;
	tree.dir = root / "coop" / "nitrofs";
	tree.layered = true;
	tree.baseVariant = "en";
	tree.module = "coop";
	tree.origin = "module coop";
	tree.variantLayer = "french";

	const auto files = rom::sweepFileTrees({ tree }, "fr");
	check(sourceOf(files, root, "uiStudio/title.bin") == "coop/nitrofs/french/uiStudio/title.bin",
		"the mapped layer supplies what it has");
	check(sourceOf(files, root, "z_new/coop/sound.nwav") == "coop/nitrofs/en/z_new/coop/sound.nwav",
		"and the module's own base layer still fills the gaps");
}

// --- components ------------------------------------------------------------

static void testDisabledComponentSubtractsItsFiles()
{
	const fs::path root = buildFixture();
	std::vector<rom::FileTree> trees = fixtureTrees(root);
	trees[0].components.push_back(rom::FileTree::Component{
		"CustomWorldUnlock", false, { "enemy/*.nsbmd" } });

	const auto files = rom::sweepFileTrees(trees, "en");
	check(sourceOf(files, root, "enemy/sign.nsbmd").empty(),
		"a disabled component's files: patterns subtract from its own tree");
	check(!sourceOf(files, root, "uiStudio/title.bin").empty(),
		"and leave the rest of the tree alone");
}

static void testDisablingOneModuleLetsAnotherThrough()
{
	const fs::path root = buildFixture();
	std::vector<rom::FileTree> trees = fixtureTrees(root);
	trees[0].components.push_back(rom::FileTree::Component{
		"Title", false, { "uiStudio/title.bin" } });

	// With coop's claim withdrawn, message's translation is no longer shadowed.
	const auto files = rom::sweepFileTrees(trees, "fr");
	check(sourceOf(files, root, "uiStudio/title.bin") == "message/nitrofs/fr/uiStudio/title.bin",
		"subtracting the winner promotes the next module's copy");
}

static void testEnabledComponentOnlyLabels()
{
	const fs::path root = buildFixture();
	std::vector<rom::FileTree> trees = fixtureTrees(root);
	trees[0].components.push_back(rom::FileTree::Component{
		"CustomWorldUnlock", true, { "enemy/*.nsbmd" } });

	const auto files = rom::sweepFileTrees(trees, "en");
	const config::FileConfig* sign = entry(files, "enemy/sign.nsbmd");
	check(sign != nullptr && sign->component == "CustomWorldUnlock",
		"an enabled component labels what it claims without removing it");
}

// --- destinations ----------------------------------------------------------

static void testIntoPrefixesTheDestination()
{
	const fs::path root = buildFixture();
	rom::FileTree tree;
	tree.dir = root / "coop" / "nitrofs" / "en";
	tree.into = "data";
	tree.module = "coop";
	tree.origin = "module coop";

	const auto files = rom::sweepFileTrees({ tree }, "en");
	check(sourceOf(files, root, "data/uiStudio/title.bin") == "coop/nitrofs/en/uiStudio/title.bin",
		"into: prefixes every destination the tree produces");
}

// A directory cannot also be a file, so an archive has to be spelled as a
// folder for its members to live in the tree next to everything else. The
// convention is load-bearing: it is how the only NARC edit either real project
// makes is written down.
static void testNarcDirectoryBecomesAnArchiveDestination()
{
	const fs::path root = fs::temp_directory_path() / "ncp_file_tree_test" / "narc";
	fs::remove_all(root);
	write(root / "fr" / "ARCHIVE" / "menu_title_narc" / "menu" / "title" / "USA" / "vs.bmg", "x");
	write(root / "fr" / "ARCHIVE" / "plain.narc", "y");

	rom::FileTree tree;
	tree.dir = root;
	tree.layered = true;
	tree.baseVariant = "en";
	tree.origin = "module message";

	const auto files = rom::sweepFileTrees({ tree }, "fr");
	check(sourceOf(files, root, "ARCHIVE/menu_title.narc!menu/title/USA/vs.bmg")
		== "fr/ARCHIVE/menu_title_narc/menu/title/USA/vs.bmg",
		"a _narc directory names a member of the archive beside it");
	check(sourceOf(files, root, "ARCHIVE/plain.narc") == "fr/ARCHIVE/plain.narc",
		"an archive replaced whole is still an ordinary destination");
}

// Only the first segment is read that way, and only a directory: an archive
// inside an archive is not something the reader opens, and a file whose own
// name ends in _narc is a file.
static void testNarcConventionIsNotGreedy()
{
	const fs::path root = fs::temp_directory_path() / "ncp_file_tree_test" / "narc2";
	fs::remove_all(root);
	write(root / "a_narc" / "inner" / "b_narc" / "c.bin", "x");
	write(root / "leaf_narc", "y");

	rom::FileTree tree;
	tree.dir = root;
	tree.origin = "the project";

	const auto files = rom::sweepFileTrees({ tree }, "");
	check(sourceOf(files, root, "a.narc!inner/b_narc/c.bin") == "a_narc/inner/b_narc/c.bin",
		"only the outermost _narc directory is read as an archive");
	check(sourceOf(files, root, "leaf_narc") == "leaf_narc",
		"a file whose name ends in _narc is a file");
}

static void testAnUnlayeredTreeIgnoresTheVariant()
{
	const fs::path root = buildFixture();
	rom::FileTree tree;
	tree.dir = root / "coop" / "nitrofs" / "en";
	tree.module = "coop";
	tree.origin = "module coop";

	const auto files = rom::sweepFileTrees({ tree }, "fr");
	check(sourceOf(files, root, "uiStudio/title.bin") == "coop/nitrofs/en/uiStudio/title.bin",
		"an unlayered tree contributes the same files to every variant");
}

static void testAMissingTreeDirectoryIsAnError()
{
	const fs::path root = buildFixture();
	rom::FileTree tree;
	tree.dir = root / "coop" / "nowhere";
	tree.origin = "module coop";

	bool threw = false;
	try { (void)rom::sweepFileTrees({ tree }, "en"); }
	catch (const std::exception&) { threw = true; }
	check(threw, "a declared tree directory that does not exist is reported");
}

static void testAnUnnameablePathIsAnError()
{
	const fs::path root = buildFixture();
	write(root / "bad" / "nitrofs" / std::string(200, 'x'), "too long");

	rom::FileTree tree;
	tree.dir = root / "bad" / "nitrofs";
	tree.origin = "module bad";

	bool threw = false;
	try { (void)rom::sweepFileTrees({ tree }, "en"); }
	catch (const std::exception&) { threw = true; }
	check(threw, "a swept file a NitroFS name cannot hold is reported, not skipped");
}

// --- determinism -----------------------------------------------------------

static void testResultIsSortedAndStable()
{
	const fs::path root = buildFixture();
	const auto first = rom::sweepFileTrees(fixtureTrees(root), "fr");
	const auto second = rom::sweepFileTrees(fixtureTrees(root), "fr");

	bool sorted = true;
	for (std::size_t i = 1; i < first.size(); i++)
		sorted = sorted && first[i - 1].path < first[i].path;
	check(sorted, "destinations come back sorted, so file ids do not depend on directory order");

	bool same = first.size() == second.size();
	for (std::size_t i = 0; same && i < first.size(); i++)
		same = first[i].path == second[i].path && first[i].source == second[i].source;
	check(same, "and two sweeps of one tree agree");
}

int main()
{
	testBaseVariantFillsTheGaps();
	testVariantOverridesItsOwnBase();
	testAVariantMaySupplyWhatNoBaseDid();
	testAnUntranslatedVariantIsNotAnError();
	testEarlierModuleWinsWithinOneLayer();
	testAModuleThatNamesItsLayersDifferently();
	testAMappedLayerMustExist();
	testMappingDoesNotDisturbTheBaseLayer();
	testLayerOutranksModuleOrder();
	testDisabledComponentSubtractsItsFiles();
	testDisablingOneModuleLetsAnotherThrough();
	testEnabledComponentOnlyLabels();
	testIntoPrefixesTheDestination();
	testNarcDirectoryBecomesAnArchiveDestination();
	testNarcConventionIsNotGreedy();
	testAnUnlayeredTreeIgnoresTheVariant();
	testAMissingTreeDirectoryIsAnError();
	testAnUnnameablePathIsAnError();
	testResultIsSortedAndStable();

	fs::remove_all(fs::temp_directory_path() / "ncp_file_tree_test");

	if (g_failures == 0)
		std::cout << "All file tree tests passed.\n";
	return g_failures == 0 ? 0 : 1;
}
