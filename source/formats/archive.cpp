#include "archive.hpp"

#include <fstream>
#include <cstring>
#include <algorithm>

Archive::Archive() :
    m_hasStringTable(false)
{}

Archive::~Archive() = default;

bool Archive::load(const std::filesystem::path& archivePath)
{
    // Clear previous data
    m_data.clear();
    m_members.clear();
    m_stringTable.clear();
    m_hasStringTable = false;

    // Read the entire archive file
    std::ifstream file(archivePath, std::ios::binary | std::ios::ate);
    if (!file.is_open())
        return false;

    std::size_t fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    m_data.resize(fileSize);
    file.read(m_data.data(), fileSize);
    file.close();

    // Parse the archive
    if (!parseArchiveHeader())
        return false;

    return parseMembers();
}

void Archive::forEachMember(const std::function<bool(const ArMember&)>& callback) const
{
    for (const auto& member : m_members)
    {
        if (callback(*member))
            break;
    }
}

const ArMember* Archive::getMember(const std::string& name) const
{
    auto it = std::find_if(m_members.begin(), m_members.end(),
        [&name](const std::unique_ptr<ArMember>& member) {
            return member->name == name;
        });
    
    return (it != m_members.end()) ? it->get() : nullptr;
}

bool Archive::parseArchiveHeader()
{
    if (m_data.size() < AR_MAGIC_SIZE)
        return false;

    // Check for AR magic signature
    if (std::memcmp(m_data.data(), AR_MAGIC, AR_MAGIC_SIZE) != 0)
        return false;

    return true;
}

bool Archive::parseMembers()
{
    std::size_t offset = AR_MAGIC_SIZE;

    // Two-pass approach: First pass to collect string table, second pass to parse members
    
    // First pass: Find and parse the string table
    std::size_t firstPassOffset = offset;
    while (firstPassOffset + sizeof(ArHeader) <= m_data.size())
    {
        const ArHeader* header = reinterpret_cast<const ArHeader*>(m_data.data() + firstPassOffset);
        
        // Verify file magic
        if (std::memcmp(header->ar_fmag, AR_FMAG, AR_FMAG_SIZE) != 0)
            break;

        std::uint64_t fileSize = parseDecimalField(header->ar_size, sizeof(header->ar_size));
        std::string fileName = parseName(*header);
        
        firstPassOffset += sizeof(ArHeader);
        
        // Look for string table
        if (fileName == "//")
        {
            m_stringTable.assign(m_data.data() + firstPassOffset, fileSize);
            m_hasStringTable = true;
            break;
        }
        
        // Move to next member (with padding)
        firstPassOffset += fileSize;
        if (firstPassOffset & 1) firstPassOffset++; // AR files have 2-byte alignment
    }

    // Second pass: Parse all members now that we have the string table
    while (offset + sizeof(ArHeader) <= m_data.size())
    {
        // Parse member header
        const ArHeader* header = reinterpret_cast<const ArHeader*>(m_data.data() + offset);
        
        // Verify file magic
        if (std::memcmp(header->ar_fmag, AR_FMAG, AR_FMAG_SIZE) != 0)
            break;

        // Parse file size
        std::uint64_t fileSize = parseDecimalField(header->ar_size, sizeof(header->ar_size));
        
        // Parse filename (now with string table available)
        std::string fileName = parseName(*header);
        
        offset += sizeof(ArHeader);

        // Skip special entries like symbol table and string table
        if (fileName == "/" || fileName == "//")
        {
            // Skip to next member (with padding)
            offset += fileSize;
            if (offset & 1) offset++; // AR files have 2-byte alignment
            continue;
        }

        // Skip entries with empty names (parsing errors)
        if (fileName.empty())
        {
            offset += fileSize;
            if (offset & 1) offset++;
            continue;
        }

        // Create member entry
        auto member = std::make_unique<ArMember>();
        member->name = fileName;
        member->size = fileSize;
        member->offset = offset;
        
        // Set direct pointer to data (no copy)
        if (offset + fileSize <= m_data.size())
        {
            member->data = m_data.data() + offset;
        }
        else
        {
            member->data = nullptr;
        }
        
        m_members.push_back(std::move(member));
        
        // Move to next member (with padding)
        offset += fileSize;
        if (offset & 1) offset++; // AR files have 2-byte alignment
    }

    return true;
}

std::uint64_t Archive::parseDecimalField(const char* field, std::size_t length) const
{
    char buffer[32] = {0};
    std::memcpy(buffer, field, std::min(length, sizeof(buffer) - 1));
    
    // Remove trailing spaces
    char* end = buffer + strlen(buffer) - 1;
    while (end > buffer && *end == ' ')
    {
        *end = '\0';
        end--;
    }
    
    return std::strtoull(buffer, nullptr, 10);
}

std::string Archive::parseName(const ArHeader& header) const
{
    // Check for long filename reference (starts with '/' followed by digits)
    if (header.ar_name[0] == '/' && header.ar_name[1] != ' ' && header.ar_name[1] != '/')
    {
        // This is a reference to the string table
        if (!m_hasStringTable)
            return ""; // String table not available
            
        // Parse the offset from the name field
        char offsetStr[16] = {0};
        int i = 1; // Skip the initial '/'
        while (i < 16 && std::isdigit(header.ar_name[i]))
        {
            offsetStr[i-1] = header.ar_name[i];
            i++;
        }
        
        std::uint64_t stringOffset = std::strtoull(offsetStr, nullptr, 10);
        if (stringOffset >= m_stringTable.size())
            return ""; // Invalid offset
            
        // Find the null-terminated string in the string table
        const char* start = m_stringTable.c_str() + stringOffset;
        const char* end = strchr(start, '/');
        if (!end)
        {
            // Some string tables use newlines instead of '/' as terminators
            end = strchr(start, '\n');
            if (!end)
                end = start + strlen(start);
        }
            
        return std::string(start, end - start);
    }
    
    // Handle special entries
    if (header.ar_name[0] == '/' && header.ar_name[1] == ' ')
        return "/";  // Symbol table
    if (header.ar_name[0] == '/' && header.ar_name[1] == '/')
        return "//"; // String table
    
    // Regular filename (padded with spaces, terminated by '/' or space)
    std::string name;
    for (int i = 0; i < 16; i++)
    {
        char c = header.ar_name[i];
        if (c == '/' || c == ' ' || c == '\0')
            break;
        name += c;
    }
    
    return name;
}
