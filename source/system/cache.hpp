#pragma once

#include <unordered_map>
#include <memory>
#include <filesystem>
#include <string>

#include "../formats/elf.hpp"
#include "../formats/archive.hpp"

namespace ncp {
namespace cache {

/**
 * Cache entry for loaded ELF files to manage ownership.
 */
struct ElfCacheEntry {
    std::unique_ptr<Elf32> elf;
    std::filesystem::path path;
};

/**
 * Cache entry for archive contents to manage ownership.
 */
struct ArchiveCacheEntry {
    std::unique_ptr<Archive> archive;
    std::filesystem::path path;
};

/**
 * Global cache manager for ELF files and archives.
 * This provides centralized cache management for loaded files,
 * ensuring proper ownership and lifetime management.
 */
class CacheManager {
public:
    /**
     * Get the global singleton instance of the cache manager.
     */
    static CacheManager& getInstance();

    /**
     * Get or load an ELF file.
     * @param filePath Path to the ELF file
     * @return Pointer to ELF32 object (throws on failure)
     */
    Elf32* getOrLoadElf(const std::filesystem::path& filePath);

    /**
     * Store a pre-created ELF object (used for archive members).
     * @param filePath Virtual path for the ELF (e.g., archive.a:member.o)
     * @param elf Unique pointer to ELF object to store
     * @return Pointer to the stored ELF32 object
     */
    Elf32* storeElf(const std::filesystem::path& filePath, std::unique_ptr<Elf32> elf);

    /**
     * Get or load an archive.
     * @param archivePath Path to the archive file
     * @return Pointer to Archive (throws on failure)
     */
    Archive* getOrLoadArchive(const std::filesystem::path& archivePath);

    /**
     * Clear all cache to free memory.
     */
    void clearCaches();

    /**
     * Clear only ELF cache to free memory.
     */
    void clearElfCache();

    /**
     * Clear only archive cache to free memory.
     */
    void clearArchiveCache();

    /**
     * Get cache statistics for debugging.
     */
    struct CacheStats {
        size_t elfCacheSize;
        size_t archiveCacheSize;
    };
    CacheStats getCacheStats() const;

private:
    CacheManager() = default;
    ~CacheManager() = default;
    
    // Prevent copying
    CacheManager(const CacheManager&) = delete;
    CacheManager& operator=(const CacheManager&) = delete;

    std::unordered_map<std::string, ElfCacheEntry> m_elfCache;
    std::unordered_map<std::string, ArchiveCacheEntry> m_archiveCache;
};

} // namespace cache
} // namespace ncp
