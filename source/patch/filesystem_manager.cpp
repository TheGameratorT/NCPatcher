#include "filesystem_manager.hpp"

#include <fstream>
#include <sstream>
#include <cstring>

#include "../app/application.hpp"
#include "../system/log.hpp"
#include "../system/except.hpp"
#include "../config/buildconfig.hpp"
#include "../ndsbin/overlaybin.hpp"

namespace fs = std::filesystem;

namespace ncp::patch {

FileSystemManager::FileSystemManager() = default;
FileSystemManager::~FileSystemManager() = default;

void FileSystemManager::initialize(
    const BuildTarget& target,
    const std::filesystem::path& buildDir,
    const HeaderBin& header
)
{
    m_target = &target;
    m_buildDir = &buildDir;
    m_header = &header;
}

void FileSystemManager::createBuildDirectory()
{
    fs::current_path(ncp::Application::getWorkPath());
    const fs::path& buildDir = *m_buildDir;
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
    fs::current_path(ncp::Application::getWorkPath());
    const fs::path& bakDir = BuildConfig::getBackupDir();
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

    fs::current_path(ncp::Application::getWorkPath());

    fs::path bakBinName = BuildConfig::getBackupDir() / binName;

    m_arm = std::make_unique<ArmBin>();
    if (fs::exists(bakBinName)) //has backup
    {
        m_arm->load(bakBinName, entryAddress, ramAddress, autoLoadListHookOff, isArm9);
    }
    else //has no backup
    {
        fs::current_path(ncp::Application::getRomPath());
        m_arm->load(binName, entryAddress, ramAddress, autoLoadListHookOff, isArm9);
        const std::vector<u8>& bytes = m_arm->data();

        fs::current_path(ncp::Application::getWorkPath());
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

    fs::current_path(ncp::Application::getRomPath());
    std::ofstream outputFile(binName, std::ios::binary);
    if (!outputFile.is_open())
        throw ncp::file_error(binName, ncp::file_error::write);
    outputFile.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    outputFile.close();
}

void FileSystemManager::loadOverlayTableBin()
{
    Log::info("Loading overlay table...");

    const char* binName = m_target->getArm9() ? "arm9ovt.bin" : "arm7ovt.bin";

    fs::current_path(ncp::Application::getWorkPath());

    fs::path bakBinName = BuildConfig::getBackupDir() / binName;

    fs::path workBinName;
    if (fs::exists(bakBinName)) //has backup
    {
        workBinName = bakBinName;
    }
    else //has no backup
    {
        fs::current_path(ncp::Application::getRomPath());
        if (!fs::exists(binName))
            throw ncp::file_error(binName, ncp::file_error::find);
        workBinName = binName;
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

    m_bakOvtEntries.resize(m_ovtEntries.size());
    std::memcpy(m_bakOvtEntries.data(), m_ovtEntries.data(), m_ovtEntries.size() * sizeof(OvtEntry));
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
    {
        fs::current_path(ncp::Application::getWorkPath());
        saveOvtEntries(m_bakOvtEntries, BuildConfig::getBackupDir() / binName);
    }

    fs::current_path(ncp::Application::getRomPath());
    saveOvtEntries(m_ovtEntries, binName);
}

OverlayBin* FileSystemManager::loadOverlayBin(std::size_t ovID)
{
    std::string prefix = m_target->getArm9() ? "overlay9" : "overlay7";

    fs::current_path(ncp::Application::getWorkPath());

    fs::path binName = fs::path(prefix) / (prefix + "_" + std::to_string(ovID) + ".bin");
    fs::path bakBinName = BuildConfig::getBackupDir() / binName;

    OvtEntry& ovte = m_ovtEntries[ovID];

    auto* overlay = new OverlayBin();
    if (fs::exists(bakBinName)) //has backup
    {
        overlay->load(bakBinName, ovte.ramAddress, ovte.flag & OVERLAY_FLAG_COMP, ovID);
        ovte.flag = 0;
    }
    else //has no backup
    {
        fs::current_path(ncp::Application::getRomPath());
        overlay->load(binName, ovte.ramAddress, ovte.flag & OVERLAY_FLAG_COMP, ovID);
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

        fs::current_path(ncp::Application::getRomPath());
        saveOvData(ov->data(), binName);

        if (!ov->backupData().empty())
        {
            fs::current_path(ncp::Application::getWorkPath());
            saveOvData(ov->backupData(), BuildConfig::getBackupDir() / binName);
        }
    }
}

} // namespace ncp::patch
