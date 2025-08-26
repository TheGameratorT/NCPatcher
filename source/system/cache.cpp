#include "cache.hpp"

#include "../system/log.hpp"
#include "../system/except.hpp"
#include "../app/application.hpp"

#include <stdexcept>

namespace ncp {
namespace cache {

CacheManager& CacheManager::getInstance() {
    static CacheManager instance;
    return instance;
}

Elf32* CacheManager::getOrLoadElf(const std::filesystem::path& filePath) {
    std::string pathStr = filePath.string();
    
    // Check if we have this ELF stored
    auto elfIt = m_elfCache.find(pathStr);
    if (elfIt != m_elfCache.end()) {
        return elfIt->second.elf.get();
    }

    // This method should only handle regular ELF files, not archive members
    if (!std::filesystem::exists(filePath)) {
        throw ncp::file_error(filePath, ncp::file_error::find);
    }
    
    // Load the ELF file
    auto elf = std::make_unique<Elf32>();
    if (!elf->load(filePath)) {
        throw ncp::file_error(filePath, ncp::file_error::read);
    }

    // Store the loaded ELF
    ElfCacheEntry elfCache;
    elfCache.elf = std::move(elf);
    elfCache.path = filePath;
    
    Elf32* result = elfCache.elf.get();
    m_elfCache[pathStr] = std::move(elfCache);
    
    return result;
}

Elf32* CacheManager::storeElf(const std::filesystem::path& filePath, std::unique_ptr<Elf32> elf) {
    std::string pathStr = filePath.string();
    
    // Check if we already have this ELF stored
    auto elfIt = m_elfCache.find(pathStr);
    if (elfIt != m_elfCache.end()) {
        return elfIt->second.elf.get();
    }

    // Store the ELF
    ElfCacheEntry elfCache;
    elfCache.path = filePath;
    Elf32* result = elf.get();
    elfCache.elf = std::move(elf);
    
    m_elfCache[pathStr] = std::move(elfCache);
    
    return result;
}

Archive* CacheManager::getOrLoadArchive(const std::filesystem::path& archivePath) {
    std::string archivePathStr = archivePath.string();
    
    // Check if we have this archive stored
    auto archiveIt = m_archiveCache.find(archivePathStr);
    if (archiveIt != m_archiveCache.end()) {
        return archiveIt->second.archive.get();
    }
    
    // Load and store the archive
    auto archive = std::make_unique<Archive>();
    if (!archive->load(archivePath)) {
        throw ncp::file_error(archivePath, ncp::file_error::read);
    }

    // Store the loaded Archive
    ArchiveCacheEntry archiveCache;
    archiveCache.archive = std::move(archive);
    archiveCache.path = archivePath;
    
    Archive* result = archiveCache.archive.get();
    m_archiveCache[archivePathStr] = std::move(archiveCache);
    
    return result;
}

void CacheManager::clearCaches() {
    m_elfCache.clear();
    m_archiveCache.clear();
}

void CacheManager::clearElfCache() {
    m_elfCache.clear();
}

void CacheManager::clearArchiveCache() {
    m_archiveCache.clear();
}

CacheManager::CacheStats CacheManager::getCacheStats() const {
    CacheStats stats;
    stats.elfCacheSize = m_elfCache.size();
    stats.archiveCacheSize = m_archiveCache.size();
    return stats;
}

} // namespace cache
} // namespace ncp
