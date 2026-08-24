#include "filesystem_manager.hpp"

#include <sstream>

#include "../system/log.hpp"
#include "../system/except.hpp"
#include "../system/message.hpp"
#include "../formats/blz.hpp"

namespace fs = std::filesystem;

namespace ncp::patch {

using ncp::rom::BackupStore;
using ncp::rom::OverlayEntry;
using ncp::rom::OverlayTable;

FileSystemManager::FileSystemManager() = default;
FileSystemManager::~FileSystemManager() = default;

void FileSystemManager::initialize(
	const BuildTarget& target,
	const ncp::Context& ctx,
	ncp::rom::RomAccessor& rom
)
{
	m_target = &target;
	m_ctx = &ctx;
	m_paths = &ctx.paths;
	m_rom = &rom;
	m_backup = std::make_unique<BackupStore>(ctx.backupDir());
}

bool FileSystemManager::isArm9() const
{
	return m_target->getArm9();
}

void FileSystemManager::createBuildDirectory()
{
	const fs::path& buildDir = m_paths->buildDir;
	if (!fs::exists(buildDir))
	{
		if (!fs::create_directories(buildDir))
		{
			std::ostringstream oss;
			oss << "Could not create build directory: " << OSTR(buildDir);
			throw ncp::exception(oss.str());
		}
	}
}

void FileSystemManager::createBackupDirectory()
{
	m_backup->createDirectories(isArm9());
}

void FileSystemManager::loadArmBin()
{
	const bool arm9 = isArm9();

	const ncp::rom::ArmBinaryInfo info = m_rom->header().arm(arm9);
	const u32 autoLoadListHookOff = m_rom->header().autoLoadListHookAddress(arm9);

	const std::string key = BackupStore::armKey(arm9);

	m_arm = std::make_unique<ArmBin>();
	if (m_backup->has(key))
	{
		m_arm->load(m_backup->read(key), info.entryAddress, info.ramAddress, autoLoadListHookOff, arm9);
	}
	else
	{
		// The pristine bytes are saved before anything is patched into them,
		// and everything afterwards reads from that copy. A build that skipped
		// this would be patching its own previous output.
		std::vector<u8> bytes = m_rom->readArm(arm9);
		m_backup->write(key, bytes);
		m_arm->load(std::move(bytes), info.entryAddress, info.ramAddress, autoLoadListHookOff, arm9);
	}
}

void FileSystemManager::saveArmBin()
{
	const bool arm9 = isArm9();
	const std::string name = m_rom->nameOfArm(arm9);
	const std::vector<u8>& bytes = m_arm->data();

	reportWrite("arm", name, -1, bytes.size(), true, nullptr);
	m_rom->writeArm(arm9, bytes);
}

void FileSystemManager::loadOverlayTableBin()
{
	Log::info("Loading overlay table...");

	const bool arm9 = isArm9();
	const std::string key = BackupStore::overlayTableKey(arm9);

	m_ovt = m_backup->has(key)
		? OverlayTable::parse(m_backup->read(key))
		: m_rom->readOverlayTable(arm9);

	// Plain assignment, not resize+memcpy: an empty table (arm7 usually has one)
	// means both data() pointers are null, and memcpy forbids that even for a
	// zero length.
	m_bakOvt = m_ovt;
}

void FileSystemManager::saveOverlayTableBin()
{
	const bool arm9 = isArm9();
	const std::string name = m_rom->nameOfOverlayTable(arm9);

	reportWrite("overlay-table", name, -1, m_ovt.byteSize(), true, nullptr);

	if (m_bakOvtChanged)
		m_backup->write(BackupStore::overlayTableKey(arm9), m_bakOvt.serialize());

	m_rom->writeOverlayTable(arm9, m_ovt);
}

OverlayBin* FileSystemManager::loadOverlayBin(std::size_t ovID)
{
	const bool arm9 = isArm9();
	const std::string key = BackupStore::overlayKey(arm9, u32(ovID));

	OverlayEntry& ovte = m_ovt.entries()[ovID];
	const bool wasCompressed = ovte.compressed();

	auto overlay = std::make_unique<OverlayBin>();
	if (m_backup->has(key))
	{
		overlay->load(m_backup->read(key), ovte.ramAddress, wasCompressed, int(ovID));
		ovte.flags = 0;
	}
	else
	{
		overlay->load(m_rom->readOverlay(arm9, u32(ovID)), ovte.ramAddress, wasCompressed, int(ovID));
		ovte.flags = 0;

		// The backup is the decompressed form, and the backed-up table row has
		// its compression flag cleared to match. Storing the compressed bytes
		// with a cleared flag -- or the plain bytes with it set -- would make
		// the next build read nonsense.
		overlay->backupData() = overlay->data();

		m_bakOvt.entries()[ovID].flags = 0;
		m_bakOvtChanged = true;
	}

	OverlayBin* result = overlay.get();
	m_loadedOverlays.emplace(ovID, std::move(overlay));
	return result;
}

OverlayBin* FileSystemManager::getOverlay(std::size_t ovID)
{
	const auto it = m_loadedOverlays.find(ovID);
	if (it != m_loadedOverlays.end())
		return it->second.get();
	return loadOverlayBin(ovID);
}

void FileSystemManager::saveOverlayBins()
{
	const bool arm9 = isArm9();

	for (auto& [ovID, ov] : m_loadedOverlays)
	{
		const std::string name = m_rom->nameOfOverlay(arm9, u32(ovID));
		const bool existed = m_rom->hasOverlay(arm9, u32(ovID));

		OverlayEntry& entry = m_ovt.entries()[ovID];

		// `compress: true` on the region was parsed and then ignored for as
		// long as the key has existed, so an overlay it named went into the ROM
		// uncompressed and the project silently got a bigger ROM than it asked
		// for. The ram size stays the decompressed length -- that is what the
		// loader allocates -- while the table's own 24-bit field carries what
		// is actually stored.
		const BuildTarget::Region* region = m_target->getRegionByDestination(int(ovID));
		std::vector<u8> stored;
		if (region != nullptr && region->compress)
			stored = BLZ::compress(ov->data());

		if (!stored.empty())
		{
			entry.compressedSize = u32(stored.size());
			entry.setCompressed(true);
		}
		else
		{
			// Either the region did not ask for compression, or the data did
			// not get smaller. Storing it "compressed" anyway would cost the
			// game a decompression pass to end up with a bigger file.
			stored = ov->data();
			entry.compressedSize = 0;
			entry.setCompressed(false);
		}

		reportWrite("overlay", name, int(ovID), stored.size(), existed, &entry);

		m_rom->writeOverlay(arm9, u32(ovID), stored);

		if (!ov->backupData().empty())
			m_backup->write(BackupStore::overlayKey(arm9, u32(ovID)), ov->backupData());
	}
}

// Announces a ROM file this build is about to write.
//
// Called before the write so that `action` can still tell an overlay this build
// invented from one it edited -- afterwards every file exists and the question
// can no longer be answered. That distinction is the one NSMB-Editor needs:
// re-importing a patched directory currently throws when it looks up an overlay
// by a name its own filesystem has never been told about.
void FileSystemManager::reportWrite(
	const char* kind, const std::string& name, int id,
	std::size_t size, bool existed, const OverlayEntry* entry) const
{
	msg::Artifact artifact;
	artifact.kind = kind;
	artifact.proc = isArm9() ? "arm9" : "arm7";
	artifact.action = existed ? "modified" : "created";
	artifact.name = name;
	artifact.id = id;
	artifact.size = static_cast<long long>(size);

	if (entry != nullptr)
	{
		artifact.ramAddress = entry->ramAddress;
		artifact.hasRamAddress = true;
		artifact.fileId = int(entry->fileId);
	}

	msg::artifact(std::move(artifact));
}

} // namespace ncp::patch
