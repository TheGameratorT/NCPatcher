#pragma once

#include <filesystem>
#include <vector>
#include <memory>
#include <unordered_map>

#include "../utils/types.hpp"
#include "../ndsbin/headerbin.hpp"
#include "../ndsbin/armbin.hpp"
#include "../ndsbin/overlaybin.hpp"
#include "../config/buildtarget.hpp"

class FileSystemManager
{
public:
    FileSystemManager();
    ~FileSystemManager();

    void initialize(
        const BuildTarget& target,
        const std::filesystem::path& buildDir,
        const HeaderBin& header
    );

    void createBuildDirectory();
    void createBackupDirectory();

    void loadArmBin();
    void saveArmBin();
    
    void loadOverlayTableBin();
    void saveOverlayTableBin();
    
    OverlayBin* loadOverlayBin(std::size_t ovID);
    OverlayBin* getOverlay(std::size_t ovID);
    void saveOverlayBins();

    [[nodiscard]] inline ArmBin* getArm() const { return m_arm.get(); }
    std::vector<OvtEntry>& getOvtEntries() { return m_ovtEntries; }
    const std::vector<OvtEntry>& getOvtEntries() const { return m_ovtEntries; }
    const std::unordered_map<std::size_t, std::unique_ptr<OverlayBin>>& getLoadedOverlays() const { return m_loadedOverlays; }

private:
    const BuildTarget* m_target;
    const std::filesystem::path* m_buildDir;
    const HeaderBin* m_header;
    
    std::unique_ptr<ArmBin> m_arm;
    std::unordered_map<std::size_t, std::unique_ptr<OverlayBin>> m_loadedOverlays;
    std::vector<OvtEntry> m_ovtEntries;
    std::vector<OvtEntry> m_bakOvtEntries;
    bool m_bakOvtChanged = false;
};
