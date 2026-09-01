// Tests for source/rom/file_manifest.cpp: the ROM file table a build writes
// out for whoever has to name a file by number.
//
// The interesting part is not the listing, which the accessor already does. It
// is the fold: by the time the manifest is written a file the build created and
// a file it replaced look identical in the ROM, so the only thing that tells
// them apart is the set of ids insertion reported as new. Getting that backwards
// would label thirteen appended files as edits of files that never existed.

#include "../source/rom/file_manifest.hpp"

#include <iostream>
#include <sstream>
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

static bool contains(const std::string& text, std::string_view needle)
{
	return text.find(needle) != std::string::npos;
}

// Only listNitroFiles is exercised; everything else is here because RomAccessor
// is an interface and C++ wants the whole of it.
class FakeRom final : public rom::RomAccessor
{
public:
	std::vector<rom::NitroFileInfo> files;

	[[nodiscard]] const fs::path& location() const override { return m_location; }
	[[nodiscard]] std::string nameOfArm(bool) const override { return {}; }
	[[nodiscard]] std::string nameOfOverlayTable(bool) const override { return {}; }
	[[nodiscard]] std::string nameOfOverlay(bool, u32) const override { return {}; }
	[[nodiscard]] const rom::Header& header() const override { return m_header; }
	[[nodiscard]] rom::Header& header() override { return m_header; }
	[[nodiscard]] std::vector<u8> readArm(bool) override { return {}; }
	void writeArm(bool, std::span<const u8>) override {}
	[[nodiscard]] rom::OverlayTable readOverlayTable(bool) override { return {}; }
	void writeOverlayTable(bool, const rom::OverlayTable&) override {}
	[[nodiscard]] bool hasOverlay(bool, u32) const override { return false; }
	[[nodiscard]] std::vector<u8> readOverlay(bool, u32) override { return {}; }
	void writeOverlay(bool, u32, std::span<const u8>) override {}
	u32 createOverlay(bool, u32, std::span<const u8>) override { return 0; }
	[[nodiscard]] int findNitroFile(std::string_view) const override { return -1; }
	[[nodiscard]] std::vector<u8> readNitroFile(std::string_view) override { return {}; }
	u32 replaceNitroFile(std::string_view, std::span<const u8>) override { return 0; }
	u32 addNitroFile(std::string_view, std::span<const u8>) override { return 0; }
	void renameNitroFile(u32, std::string_view) override {}
	[[nodiscard]] std::string nitroFilePath(u32) const override { return {}; }
	[[nodiscard]] rom::NitroFs nitroFs() const override { return {}; }
	[[nodiscard]] u32 nextNitroFileId() const override { return 0; }
	[[nodiscard]] bool hasBanner() const override { return false; }
	[[nodiscard]] std::vector<u8> readBanner() override { return {}; }
	void writeBanner(std::span<const u8>) override {}
	[[nodiscard]] std::vector<rom::NitroFileInfo> listNitroFiles() const override { return files; }
	void commit() override {}

private:
	fs::path m_location;
	rom::Header m_header;
};

static const rom::ManifestEntry* entry(const std::vector<rom::ManifestEntry>& entries, u32 id)
{
	for (const rom::ManifestEntry& e : entries)
	{
		if (e.id == id)
			return &e;
	}
	return nullptr;
}

int main()
{
	const fs::path root = fs::path("/project");

	FakeRom rom;
	rom.files = {
		{ 133, "sound_data.sdat", 4096, },
		{ 131, "00DUMMY", 0, },
		{ 500, "uiStudio/title.bin", 2031, },
		{ 2100, "z_new/reserved", 0, },
		{ 2101, "z_new/coop/shot.nwav", 64, },
	};

	std::vector<config::FileConfig> files;
	{
		config::FileConfig replaced;
		replaced.path = "uiStudio/title.bin";
		replaced.source = root / "modules/coop/nitrofs/en/uiStudio/title.bin";
		replaced.module = "coop";
		replaced.component = "Vanilla";
		replaced.fromVariant = "en";
		files.push_back(std::move(replaced));

		config::FileConfig added;
		added.path = "z_new/coop/shot.nwav";
		added.source = root / "modules/coop/nitrofs/base/z_new/coop/shot.nwav";
		added.module = "coop";
		files.push_back(std::move(added));
	}

	rom::InsertionRecord record;
	record.files = files;
	record.createdIds = { 2100, 2101 };
	const std::vector<rom::ManifestEntry> entries = rom::buildManifest(rom, record, root);

	// Every file in the table, not only the two the build wrote: a generator
	// naming files by id needs the ones it did not touch just as much.
	check(entries.size() == 5, "the manifest reports every file in the table");

	// Sorted by id, whatever order the accessor walked the tree in.
	bool sorted = true;
	for (std::size_t i = 1; i < entries.size(); i++)
		sorted = sorted && entries[i - 1].id < entries[i].id;
	check(sorted, "entries are sorted by id");

	const rom::ManifestEntry* untouched = entry(entries, 133);
	check(untouched != nullptr && untouched->action == rom::FileAction::Unchanged,
		"a file the build never wrote is unchanged");
	check(untouched != nullptr && untouched->source.empty() && untouched->module.empty(),
		"an unchanged file carries no provenance");
	check(untouched != nullptr && untouched->size == 4096, "the size comes from the ROM");

	const rom::ManifestEntry* replaced = entry(entries, 500);
	check(replaced != nullptr && replaced->action == rom::FileAction::Modified,
		"a file the build replaced is modified, not created");
	check(replaced != nullptr && replaced->module == "coop" && replaced->component == "Vanilla"
		&& replaced->fromVariant == "en", "provenance follows the resolved file config");

	// Relative to the project, because that is how the config wrote it and how
	// a diff of two manifests will read.
	check(replaced != nullptr && replaced->source == "modules/coop/nitrofs/en/uiStudio/title.bin",
		"the source is relative to the project directory");

	const rom::ManifestEntry* added = entry(entries, 2101);
	check(added != nullptr && added->action == rom::FileAction::Created,
		"an appended file is created");
	check(added != nullptr && added->fromVariant.empty(),
		"a base-layer file names no variant");

	// NCPatcher's own: created by insertion with nothing behind it, so it is
	// created without a source rather than absent from the manifest.
	const rom::ManifestEntry* reserved = entry(entries, 2100);
	check(reserved != nullptr && reserved->action == rom::FileAction::Created,
		"z_new/reserved is created");
	check(reserved != nullptr && reserved->source.empty(),
		"z_new/reserved has no source");

	// A source outside the project stays absolute rather than growing a stack
	// of `..` segments that resolve against nothing in particular.
	{
		std::vector<config::FileConfig> outside;
		config::FileConfig file;
		file.path = "uiStudio/title.bin";
		file.source = fs::path("/elsewhere/store/title.bin");
		outside.push_back(std::move(file));

		rom::InsertionRecord elsewhere;
		elsewhere.files = std::move(outside);
		const std::vector<rom::ManifestEntry> result = rom::buildManifest(rom, elsewhere, root);
		const rom::ManifestEntry* found = entry(result, 500);
		check(found != nullptr && found->source == "/elsewhere/store/title.bin",
			"a source outside the project stays absolute");
	}

	{
		std::ostringstream out;
		rom::writeManifest(out, entries, "en");
		const std::string text = out.str();

		check(contains(text, "\"schema\": \"ncpatcher.files/1\""), "the schema is named");
		check(contains(text, "\"variant\": \"en\""), "the variant is named");
		check(contains(text, "\"count\": 5"), "the count matches the entries");
		check(contains(text, "\"action\": \"unchanged\""), "unchanged is spelled out");
		check(contains(text, "\"action\": \"modified\""), "modified is spelled out");
		check(contains(text, "\"action\": \"created\""), "created is spelled out");
		check(contains(text, "\"from-variant\": \"en\""), "from-variant is kebab-case");

		// Two thousand unchanged entries carrying four empty strings each would
		// double the file to say nothing at all.
		check(!contains(text, "\"source\": \"\""), "empty provenance fields are omitted");
		check(!contains(text, "\"module\": \"\""), "empty module is omitted");
	}

	{
		// `rom files --json` reads a ROM off disk, which is the result of a
		// build rather than one, so there is no variant to name.
		std::ostringstream out;
		rom::writeManifest(out, entries, std::string_view());
		check(!contains(out.str(), "\"variant\""), "no variant key without a variant");
	}

	// `files plan`. The ids under z_new/ in one of these are predictions, so
	// the document has to say so on its face: a consumer that could not tell a
	// plan from a build would show a prospective id as a settled one.
	{
		std::ostringstream out;
		rom::writeManifest(out, entries, "en", true);
		check(contains(out.str(), "\"planned\": true"), "a plan says it is one");

		std::ostringstream built;
		rom::writeManifest(built, entries, "en");
		check(!contains(built.str(), "\"planned\""),
			"and a build's manifest carries no such key at all");
	}

	// A source a pre-build hook has not generated yet. The destination, the id
	// and the provenance are still right; the size is the only thing that is
	// not, and the entry says which entries those are rather than leaving a
	// reader to guess.
	{
		rom::InsertionRecord incomplete = record;
		incomplete.missingSources = { "uiStudio/title.bin" };
		const std::vector<rom::ManifestEntry> planned = rom::buildManifest(rom, incomplete, root);

		const rom::ManifestEntry* missing = entry(planned, 500);
		check(missing != nullptr && missing->sourceMissing,
			"a destination whose source was not there is flagged");
		check(missing != nullptr && missing->action == rom::FileAction::Modified
			&& missing->module == "coop",
			"and keeps the action and provenance the plan resolved");

		const rom::ManifestEntry* present = entry(planned, 2101);
		check(present != nullptr && !present->sourceMissing,
			"a file whose source was read is not flagged");

		std::ostringstream out;
		rom::writeManifest(out, planned, "en", true);
		check(contains(out.str(), "\"source-missing\": true"), "and the flag reaches the document");
		check(!contains(out.str(), "\"source-missing\": false"),
			"which carries the key only where it is true");
	}

	// Members of an edited archive. The container's own entry can carry no more
	// provenance than its members agree on, so without these the per-member
	// truth is discarded and an editor showing the inside of an archive has to
	// re-derive it from the module trees -- which is the duplication all of
	// this exists to remove.
	{
		rom::InsertionRecord edited;
		{
			// What insertion pushes for an archive whose members were replaced
			// but whose container no entry supplied.
			config::FileConfig summary;
			summary.path = "ARCHIVE/menu_title.narc";
			summary.module = "message";
			edited.files.push_back(std::move(summary));
		}
		edited.archiveEdits.push_back(rom::ArchiveEdit{
			"ARCHIVE/menu_title.narc", 43, "menu/title/USA/vs.bmg", 2031,
			root / "modules/message/nitrofs/fr/ARCHIVE/menu_title_narc/menu/title/USA/vs.bmg",
			"message", "Vanilla", "fr", false });
		edited.archiveEdits.push_back(rom::ArchiveEdit{
			"ARCHIVE/menu_title.narc", 14, "menu/title/USA/opening.bmg", 64,
			root / "modules/message/nitrofs/fr/ARCHIVE/menu_title_narc/menu/title/USA/opening.bmg",
			"message", "Vanilla", "fr", false });

		FakeRom archived;
		archived.files = { { 812, "ARCHIVE/menu_title.narc", 40960, } };

		const std::vector<rom::ManifestEntry> result = rom::buildManifest(archived, edited, root);
		const rom::ManifestEntry* container = entry(result, 812);
		check(container != nullptr && container->action == rom::FileAction::Modified,
			"an archive whose members were edited is modified");
		check(container != nullptr && container->members.size() == 2,
			"both edited members are recorded");

		// Sorted by index, so two runs of the same project produce the same
		// document whatever order the edits were resolved in.
		check(container != nullptr && container->members.size() == 2
			&& container->members[0].index == 14 && container->members[1].index == 43,
			"members are ordered by index");
		check(container != nullptr && !container->members.empty()
			&& container->members[0].path == "menu/title/USA/opening.bmg"
			&& container->members[0].module == "message"
			&& container->members[0].fromVariant == "fr"
			&& container->members[0].source
				== "modules/message/nitrofs/fr/ARCHIVE/menu_title_narc/menu/title/USA/opening.bmg",
			"a member carries the provenance the container cannot");

		std::ostringstream out;
		rom::writeManifest(out, result, "fr");
		const std::string text = out.str();
		check(contains(text, "\"members\""), "members reach the document");
		check(contains(text, "\"index\": 43"), "a member is addressed by index");
		check(!contains(text, "\"id\": 43"),
			"and never by id, which a member does not have");

		// Every other file in the ROM: no members key at all, rather than an
		// empty array on two thousand entries.
		FakeRom plain;
		plain.files = { { 133, "sound_data.sdat", 4096, } };
		std::ostringstream untouched;
		rom::writeManifest(untouched, rom::buildManifest(plain, {}, root), std::string_view());
		check(!contains(untouched.str(), "\"members\""),
			"a file that is not an edited archive carries no members key");
	}

	if (g_failures == 0)
		std::cout << "All file manifest tests passed\n";
	return g_failures == 0 ? 0 : 1;
}
