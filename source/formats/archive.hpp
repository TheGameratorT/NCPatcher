#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include <memory>

// AR archive header constants
#define AR_MAGIC "!<arch>\n"
#define AR_MAGIC_SIZE 8
#define AR_FMAG "`\n"
#define AR_FMAG_SIZE 2

// AR file header structure (60 bytes)
struct ArHeader
{
    char ar_name[16];    // File name
    char ar_date[12];    // File modification timestamp
    char ar_uid[6];      // User ID
    char ar_gid[6];      // Group ID  
    char ar_mode[8];     // File permissions
    char ar_size[10];    // File size in bytes
    char ar_fmag[2];     // File magic signature ("`\n")
};

/**
 * Represents a single object file within an AR archive
 */
struct ArMember
{
    std::string name;        // Object file name
    std::uint64_t size;      // Size of the object file data
    std::uint64_t offset;    // Offset in the archive where the object file data starts
    const char* data;        // Direct pointer to object file data in archive (no copy)
};

/**
 * AR Archive reader class
 * Handles reading of UNIX AR archive files (typically .a static library files)
 */
class Archive
{
public:
    Archive();
    ~Archive();

    // Load an AR archive file
    bool load(const std::filesystem::path& archivePath);

    // Get all members in the archive
    const std::vector<std::unique_ptr<ArMember>>& getMembers() const { return m_members; }

    // Iterate over each member in the archive
    void forEachMember(const std::function<bool(const ArMember&)>& callback) const;

    // Get a specific member by name
    const ArMember* getMember(const std::string& name) const;

private:
    bool parseArchiveHeader();
    bool parseMembers();
    std::uint64_t parseDecimalField(const char* field, std::size_t length) const;
    std::string parseName(const ArHeader& header) const;

    std::vector<char> m_data;  // Raw archive file data
    std::vector<std::unique_ptr<ArMember>> m_members;
    
    // String table for long filenames (if present)
    std::string m_stringTable;
    bool m_hasStringTable;
};
