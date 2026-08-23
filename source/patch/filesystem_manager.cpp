#include "filesystem_manager.hpp"

#include <fstream>
#include <sstream>
#include <cstring>

#include "../system/log.hpp"
#include "../system/except.hpp"
#include "../ndsbin/overlaybin.hpp"

namespace fs = std::filesystem;

namespace ncp::patch {

FileSystemManager::FileSystemManager() = default;
FileSystemManager::~FileSystemManager() = default;

void FileSystemManager::initialize(
    const BuildTarget& target,
    const ncp::Context& ctx,
    const HeaderBin& header
)
{
    m_target = &target;
    m_ctx = &ctx;
    m_paths = &ctx.paths;
    m_header = &header;
}

fs::path FileSystemManager::backupPath(const fs::path& relative) const
{
    return m_ctx->backupDir() / relative;
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
    const fs::path bakDir = m_ctx->backupDir();
    if (!fs::exists(bakDir))
    {
        if (!fs::create_directories(bakDir))
        {
            std::ostringstream oss;
            oss << "Could not create backup directory: " << OSTR(bakDir);
            throw ncp::exception(oss.str());
        }
    }

    const char* prefix = m_target->getArm9() ? "overlay9" : "overlay7";
    fs::path bakOvDir = bakDir / prefix;
    if (!fs::exists(bakOvDir))
    {
        if (!fs::create_directories(bakOvDir))
        {
            std::ostringstream oss;
            oss << "Could not create overlay backup directory: " << OSTR(bakOvDir);
            throw ncp::exception(oss.str());
        }
    }
}

void FileSystemManager::loadArmBin()
{
    bool isArm9 = m_target->getArm9();

    const char* binName; u32 entryAddress, ramAddress, autoLoadListHookOff;
    if (isArm9)
    {
        binName = "arm9.bin";
        entryAddress = m_header->arm9.entryAddress;
        ramAddress = m_header->arm9.ramAddress;
        autoLoadListHookOff = m_header->arm9AutoLoadListHookOffset;
    }
    else
    {
        binName = "arm7.bin";
        entryAddress = m_header->arm7.entryAddress;
        ramAddress = m_header->arm7.ramAddress;
        autoLoadListHookOff = m_header->arm7AutoLoadListHookOffset;
    }

    fs::path bakBinName = backupPath(binName);

    m_arm = std::make_unique<ArmBin>();
    if (fs::exists(bakBinName)) //has backup
    {
        m_arm->load(bakBinName, entryAddress, ramAddress, autoLoadListHookOff, isArm9);
    }
    else //has no backup
    {
        m_arm->load(m_paths->rom(binName), entryAddress, ramAddress, autoLoadListHookOff, isArm9);
        const std::vector<u8>& bytes = m_arm->data();

        std::ofstream outputFile(bakBinName, std::ios::binary);
        if (!outputFile.is_open())
            throw ncp::file_error(bakBinName, ncp::file_error::write);
        outputFile.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        outputFile.close();
    }
}

void FileSystemManager::saveArmBin()
{
    const char* binName = m_target->getArm9() ? "arm9.bin" : "arm7.bin";

    const std::vector<u8>& bytes = m_arm->data();

    const fs::path romBinPath = m_paths->rom(binName);
    std::ofstream outputFile(romBinPath, std::ios::binary);
    if (!outputFile.is_open())
        throw ncp::file_error(romBinPath, ncp::file_error::write);
    outputFile.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    outputFile.close();
}

void FileSystemManager::loadOverlayTableBin()
{
    Log::info("Loading overlay table...");

    const char* binName = m_target->getArm9() ? "arm9ovt.bin" : "arm7ovt.bin";

    fs::path bakBinName = backupPath(binName);

    fs::path workBinName;
    if (fs::exists(bakBinName)) //has backup
    {
        workBinName = bakBinName;
    }
    else //has no backup
    {
        workBinName = m_paths->rom(binName);
        if (!fs::exists(workBinName))
            throw ncp::file_error(workBinName, ncp::file_error::find);
    }

    uintmax_t fileSize = fs::file_size(workBinName);
    u32 overlayCount = fileSize / sizeof(OvtEntry);

    m_ovtEntries.resize(overlayCount);

    std::ifstream inputFile(workBinName, std::ios::binary);
    if (!inputFile.is_open())
        throw ncp::file_error(workBinName, ncp::file_error::read);
    for (u32 i = 0; i < overlayCount; i++)
        inputFile.read(reinterpret_cast<char*>(&m_ovtEntries[i]), sizeof(OvtEntry));
    inputFile.close();

    // Plain assignment, not resize+memcpy: an empty table (arm7 usually has one)
    // means both data() pointers are null, and memcpy forbids that even for a
    // zero length.
    m_bakOvtEntries = m_ovtEntries;
}

void FileSystemManager::saveOverlayTableBin()
{
    auto saveOvtEntries = [](const std::vector<OvtEntry>& ovtEntries, const fs::path& filePath){
        std::ofstream outputFile(filePath, std::ios::binary);
        if (!outputFile.is_open())
            throw ncp::file_error(filePath, ncp::file_error::write);
        outputFile.write(reinterpret_cast<const char*>(ovtEntries.data()), ovtEntries.size() * sizeof(OvtEntry));
        outputFile.close();
    };

    const char* binName = m_target->getArm9() ? "arm9ovt.bin" : "arm7ovt.bin";

    if (m_bakOvtChanged)
        saveOvtEntries(m_bakOvtEntries, backupPath(binName));

    saveOvtEntries(m_ovtEntries, m_paths->rom(binName));
}

OverlayBin* FileSystemManager::loadOverlayBin(std::size_t ovID)
{
    std::string prefix = m_target->getArm9() ? "overlay9" : "overlay7";

    fs::path binName = fs::path(prefix) / (prefix + "_" + std::to_string(ovID) + ".bin");
    fs::path bakBinName = backupPath(binName);

    OvtEntry& ovte = m_ovtEntries[ovID];

    auto* overlay = new OverlayBin();
    if (fs::exists(bakBinName)) //has backup
    {
        overlay->load(bakBinName, ovte.ramAddress, ovte.flag & OVERLAY_FLAG_COMP, ovID);
        ovte.flag = 0;
    }
    else //has no backup
    {
        overlay->load(m_paths->rom(binName), ovte.ramAddress, ovte.flag & OVERLAY_FLAG_COMP, ovID);
        ovte.flag = 0;
        const std::vector<u8>& bytes = overlay->data();

        std::vector<u8>& backupBytes = overlay->backupData();
        backupBytes.resize(bytes.size());
        std::memcpy(backupBytes.data(), bytes.data(), bytes.size());

        m_bakOvtEntries[ovID].flag = 0;
        m_bakOvtChanged = true;
    }

    m_loadedOverlays.emplace(ovID, std::unique_ptr<OverlayBin>(overlay));
    return overlay;
}

OverlayBin* FileSystemManager::getOverlay(std::size_t ovID)
{
    for (auto& [id, ov] : m_loadedOverlays)
    {
        if (id == ovID)
            return ov.get();
    }
    return loadOverlayBin(ovID);
}

void FileSystemManager::saveOverlayBins()
{
    std::string prefix = m_target->getArm9() ? "overlay9" : "overlay7";

    for (auto& [ovID, ov] : m_loadedOverlays)
    {
        fs::path binName = fs::path(prefix) / (prefix + "_" + std::to_string(ovID) + ".bin");

        auto saveOvData = [](const std::vector<u8>& ovData, const fs::path& ovFilePath){
            std::ofstream outputFile(ovFilePath, std::ios::binary);
            if (!outputFile.is_open())
                throw ncp::file_error(ovFilePath, ncp::file_error::write);
            outputFile.write(reinterpret_cast<const char*>(ovData.data()), std::streamsize(ovData.size()));
            outputFile.close();
        };

        saveOvData(ov->data(), m_paths->rom(binName));

        if (!ov->backupData().empty())
            saveOvData(ov->backupData(), backupPath(binName));
    }
}

} // namespace ncp::patch
