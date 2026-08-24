#include "../source/app/context.hpp"
#include "../source/config/buildtarget.hpp"
#include "../source/config/project_config.hpp"
#include "../source/config/rebuild_store.hpp"
#include "../source/patch/filesystem_manager.hpp"
#include "../source/rom/accessor.hpp"
#include "../source/rom/backup_store.hpp"
#include "../source/system/message.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <unordered_map>

namespace fs = std::filesystem;
using ncp::rom::OverlayEntry;
using ncp::rom::OverlayTable;

namespace {

int failures = 0;

void check(bool condition, const char* what)
{
	if (!condition)
	{
		std::cout << "FAIL: " << what << '\n';
		failures++;
	}
}

class FakeRom final : public ncp::rom::RomAccessor
{
public:
	explicit FakeRom(fs::path root) : m_root(std::move(root))
	{
		for (u32 id = 0; id < 2; id++)
		{
			OverlayEntry entry;
			entry.overlayId = id;
			entry.ramAddress = 0x02100000 + id * 0x10000;
			entry.ramSize = 4;
			entry.fileId = id;
			table.entries().push_back(entry);
			files[id] = std::vector<u8>(4, u8(id));
		}
		nextFileId = 2;
	}

	const fs::path& location() const override { return m_root; }
	std::string nameOfArm(bool arm9) const override { return arm9 ? "arm9.bin" : "arm7.bin"; }
	std::string nameOfOverlayTable(bool arm9) const override { return arm9 ? "arm9ovt.bin" : "arm7ovt.bin"; }
	std::string nameOfOverlay(bool arm9, u32 id) const override
	{
		const std::string prefix = arm9 ? "overlay9" : "overlay7";
		return prefix + "/" + prefix + "_" + std::to_string(id) + ".bin";
	}

	const ncp::rom::Header& header() const override { return m_header; }
	ncp::rom::Header& header() override { return m_header; }
	std::vector<u8> readArm(bool) override { return {}; }
	void writeArm(bool, std::span<const u8>) override {}
	OverlayTable readOverlayTable(bool) override { return table; }
	void writeOverlayTable(bool, const OverlayTable& value) override { table = value; }

	bool hasOverlay(bool, u32 id) const override
	{
		return id < table.size() && files.contains(table.entries()[id].fileId);
	}
	std::vector<u8> readOverlay(bool, u32 id) override { return files.at(table.entries().at(id).fileId); }
	void writeOverlay(bool, u32 id, std::span<const u8> data) override
	{
		files[table.entries().at(id).fileId] = std::vector<u8>(data.begin(), data.end());
		writes++;
	}
	u32 createOverlay(bool, u32, std::span<const u8> data) override
	{
		const u32 id = nextFileId++;
		files[id] = std::vector<u8>(data.begin(), data.end());
		creates++;
		return id;
	}

	int findNitroFile(std::string_view) const override { return -1; }
	u32 replaceNitroFile(std::string_view, std::span<const u8>) override { return 0; }
	u32 addNitroFile(std::string_view, std::span<const u8>) override { return 0; }
	void commit() override {}

	OverlayTable table;
	std::unordered_map<u32, std::vector<u8>> files;
	u32 nextFileId = 0;
	int creates = 0;
	int writes = 0;

private:
	fs::path m_root;
	ncp::rom::Header m_header;
};

struct Fixture
{
	explicit Fixture(const fs::path& root, FakeRom& rom)
	{
		config.backupDir.value = "backup";
		context.paths.workDir = root;
		context.paths.buildDir = root / "build";
		context.config = &config;
		context.options = &options;
		context.rebuild = &rebuild;
		BuildTargetBuilder::setArm9(target, true);

		BuildTarget::Region region{};
		region.destination = 2;
		region.mode = BuildTarget::Mode::Create;
		region.maxsize = 0x1000;
		target.regions.push_back(region);

		manager.initialize(target, context, rom);
		manager.createBackupDirectory();
		manager.loadOverlayTableBin();
	}

	ncp::config::ProjectConfig config;
	ncp::Options options;
	ncp::config::RebuildStore rebuild;
	ncp::Context context;
	BuildTarget target;
	ncp::patch::FileSystemManager manager;
};

OverlayEntry createdEntry()
{
	OverlayEntry entry;
	entry.overlayId = 2;
	entry.ramAddress = 0x023C0000;
	entry.ramSize = 4;
	entry.bssSize = 8;
	return entry;
}

} // namespace

int main()
{
	const fs::path root = fs::temp_directory_path() / "ncp_overlay_create_test";
	std::error_code ignored;
	fs::remove_all(root, ignored);
	fs::create_directories(root);

	const fs::path resultFile = root / "result.json";
	ncp::msg::configure(ncp::msg::Format::Human, resultFile);

	FakeRom rom(root);
	u32 createdFileId = 0;
	{
		Fixture fixture(root, rom);

		bool rejectedGap = false;
		OverlayEntry gap = createdEntry();
		gap.overlayId = 3;
		try { fixture.manager.createOverlayBin(gap, {}); }
		catch (const std::exception&) { rejectedGap = true; }
		check(rejectedGap, "create mode rejects a gap in the overlay table");

		const std::vector<u8> data{ 1, 2, 3, 4 };
		fixture.manager.createOverlayBin(createdEntry(), data);
		fixture.manager.saveOverlayBins();
		fixture.manager.saveOverlayTableBin();

		check(rom.creates == 1, "the first build allocates one FAT file");
		check(rom.table.size() == 3, "the first build appends one overlay-table row");
		check(rom.table.entries()[2].overlayId == 2, "the new row has the requested overlay id");
		check(rom.table.entries()[2].ramAddress == 0x023C0000, "the new row has the configured address");
		check(rom.table.entries()[2].bssSize == 8, "the new row records BSS size");
		createdFileId = rom.table.entries()[2].fileId;
		check(rom.files[createdFileId] == data, "the new FAT file contains the linked bytes");

		const fs::path backup = root / "backup" / ncp::rom::BackupStore::overlayTableKey(true);
		check(fs::exists(backup) && fs::file_size(backup) == 2 * OverlayEntry::SIZE,
			  "the pristine overlay table is backed up without the created row");
	}

	{
		Fixture fixture(root, rom);
		const std::vector<u8> rebuilt{ 5, 6, 7, 8 };
		fixture.manager.createOverlayBin(createdEntry(), rebuilt);
		fixture.manager.saveOverlayBins();
		fixture.manager.saveOverlayTableBin();

		check(rom.creates == 1, "a rebuild reuses the created overlay's FAT file");
		check(rom.writes == 1, "a rebuild replaces the existing overlay bytes");
		check(rom.table.entries()[2].fileId == createdFileId, "the reused row keeps its file id");
		check(rom.files[createdFileId] == rebuilt, "the reused file receives the rebuilt bytes");
	}

	ncp::msg::finish("ok", 0);
	std::ifstream result(resultFile);
	const std::string json((std::istreambuf_iterator<char>(result)), std::istreambuf_iterator<char>());
	check(json.find("\"action\": \"created\"") != std::string::npos,
		  "machine output reports the first overlay as created");
	check(json.find("\"file-id\": 2") != std::string::npos,
		  "the created artifact reports its allocated file id");

	fs::remove_all(root, ignored);
	if (failures == 0)
		std::cout << "overlay_create_test: all checks passed\n";
	return failures == 0 ? 0 : 1;
}
