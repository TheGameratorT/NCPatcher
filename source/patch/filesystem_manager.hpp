#pragma once

#include <filesystem>
#include <vector>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include "../utils/types.hpp"
#include "../ndsbin/armbin.hpp"
#include "../ndsbin/overlaybin.hpp"
#include "../rom/accessor.hpp"
#include "../rom/backup_store.hpp"
#include "../config/buildtarget.hpp"
#include "../app/context.hpp"

namespace ncp::patch {

// Loads the binaries a target patches and writes them back.
//
// It does no filesystem I/O of its own any more: reads and writes go through a
// rom::RomAccessor, so the same code patches an extracted directory or a .nds
// without knowing which. What it still owns is the policy that surrounds those
// reads -- patch the pristine binary, not the last build's output -- which is
// what the backup store is for.
class FileSystemManager
{
public:
	FileSystemManager();
	~FileSystemManager();

	void initialize(
		const BuildTarget& target,
		const ncp::Context& ctx,
		ncp::rom::RomAccessor& rom
	);

	void createBuildDirectory();
	void createBackupDirectory();

	void loadArmBin();
	void saveArmBin();
    
	void loadOverlayTableBin();
	void saveOverlayTableBin();
    
	OverlayBin* loadOverlayBin(std::size_t ovID);
	OverlayBin* createOverlayBin(ncp::rom::OverlayEntry entry, std::vector<u8> data);
	OverlayBin* getOverlay(std::size_t ovID);
	void saveOverlayBins();

	[[nodiscard]] inline ArmBin* getArm() const { return m_arm.get(); }
	std::vector<ncp::rom::OverlayEntry>& getOvtEntries() { return m_ovt.entries(); }
	const std::vector<ncp::rom::OverlayEntry>& getOvtEntries() const { return m_ovt.entries(); }
	const std::unordered_map<std::size_t, std::unique_ptr<OverlayBin>>& getLoadedOverlays() const { return m_loadedOverlays; }

private:
	const BuildTarget* m_target = nullptr;
	const ncp::Context* m_ctx = nullptr;
	const PathContext* m_paths = nullptr;
	ncp::rom::RomAccessor* m_rom = nullptr;
	std::unique_ptr<ncp::rom::BackupStore> m_backup;

	[[nodiscard]] bool isArm9() const;

	// Emits the machine-readable record of a ROM file about to be written.
	// `entry` is the overlay table row, where there is one.
	void reportWrite(const char* kind, const std::string& name, int id,
					 std::size_t size, bool existed, const ncp::rom::OverlayEntry* entry) const;
    
	std::unique_ptr<ArmBin> m_arm;
	std::unordered_map<std::size_t, std::unique_ptr<OverlayBin>> m_loadedOverlays;
	std::unordered_set<std::size_t> m_createdOverlays;
	ncp::rom::OverlayTable m_ovt;
	ncp::rom::OverlayTable m_bakOvt;
	bool m_bakOvtChanged = false;
};

} // namespace ncp::patch
