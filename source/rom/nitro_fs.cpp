#include "nitro_fs.hpp"

#include <algorithm>
#include <utility>
#include <sstream>

#include "../utils/endian.hpp"
#include "../system/except.hpp"
#include "../system/log.hpp"

namespace ncp::rom {

namespace {

constexpr std::size_t DIR_ROW_SIZE = 8;

// Splits "a/b/c" without allocating a vector of strings for the caller.
std::vector<std::string_view> splitPath(std::string_view path)
{
	std::vector<std::string_view> parts;
	std::size_t start = 0;
	while (start <= path.size())
	{
		const std::size_t slash = path.find('/', start);
		const std::size_t end = (slash == std::string_view::npos) ? path.size() : slash;
		if (end > start)
			parts.push_back(path.substr(start, end - start));
		if (slash == std::string_view::npos)
			break;
		start = slash + 1;
	}
	return parts;
}

bool namesEqual(std::string_view a, std::string_view b)
{
	// NitroFS names are case-sensitive on the hardware, and treating them
	// otherwise would silently resolve the wrong file on a case-insensitive
	// host.
	return a == b;
}

} // namespace

NitroFs NitroFs::parse(std::span<const u8> data)
{
	NitroFs fs;
	if (data.empty())
		return fs;

	if (data.size() < DIR_ROW_SIZE)
		throw ncp::exception("Invalid file name table: too short to hold the root directory.");

	// The root row's "parent id" is really the directory count, GBATEK's one
	// piece of overloading in this structure.
	const std::size_t dirCount = le::readU16(data, 6);
	if (dirCount == 0 || dirCount * DIR_ROW_SIZE > data.size())
	{
		std::ostringstream oss;
		oss << "Invalid file name table: it claims " << dirCount
		    << " directories, which does not fit in " << data.size() << " bytes.";
		throw ncp::exception(oss.str());
	}

	fs.m_directories.resize(dirCount);
	for (std::size_t i = 0; i < dirCount; i++)
	{
		FsDirectory& dir = fs.m_directories[i];
		dir.id = u16(FIRST_DIR_ID + i);
		const std::size_t row = i * DIR_ROW_SIZE;
		const u32 subtableOffset = le::readU32(data, row);
		dir.firstFileId = le::readU16(data, row + 4);
		dir.parentId = (i == 0) ? FIRST_DIR_ID : le::readU16(data, row + 6);

		u32 nextFileId = dir.firstFileId;
		std::size_t cursor = subtableOffset;
		while (true)
		{
			const u8 typeLength = le::readU8(data, cursor++);
			if (typeLength == 0x00)
				break;
			if (typeLength == 0x80)
				throw ncp::exception("Invalid file name table: reserved entry type 0x80.");

			const std::size_t nameLength = typeLength & 0x7F;
			le::requireRange(data, cursor, nameLength);

			FsEntry entry;
			entry.name.assign(reinterpret_cast<const char*>(data.data()) + cursor, nameLength);
			entry.isDirectory = (typeLength & 0x80) != 0;
			cursor += nameLength;

			if (entry.isDirectory)
			{
				entry.id = le::readU16(data, cursor);
				cursor += 2;
			}
			else
			{
				entry.id = u16(nextFileId++);
			}

			dir.entries.push_back(std::move(entry));
		}
	}

	return fs;
}

std::vector<u8> NitroFs::serialize() const
{
	if (m_directories.empty())
		return {};

	// Subtables follow the directory table in directory order, which is the
	// layout every DS ROM and every tool that writes one uses.
	std::vector<u8> subtables;
	std::vector<u32> subtableOffsets(m_directories.size());
	const u32 tableSize = u32(m_directories.size() * DIR_ROW_SIZE);

	for (std::size_t i = 0; i < m_directories.size(); i++)
	{
		subtableOffsets[i] = tableSize + u32(subtables.size());
		for (const FsEntry& entry : m_directories[i].entries)
		{
			if (entry.name.empty() || entry.name.size() > 0x7F)
			{
				std::ostringstream oss;
				oss << "Cannot write file name table entry " << OSTR(entry.name)
				    << ": names must be 1 to 127 bytes long.";
				throw ncp::exception(oss.str());
			}
			subtables.push_back(u8(entry.name.size() | (entry.isDirectory ? 0x80 : 0x00)));
			subtables.insert(subtables.end(), entry.name.begin(), entry.name.end());
			if (entry.isDirectory)
			{
				subtables.push_back(u8(entry.id & 0xFF));
				subtables.push_back(u8((entry.id >> 8) & 0xFF));
			}
		}
		subtables.push_back(0x00); // end of subtable
	}

	std::vector<u8> out(tableSize + subtables.size());
	std::span<u8> span(out);
	for (std::size_t i = 0; i < m_directories.size(); i++)
	{
		const FsDirectory& dir = m_directories[i];
		const std::size_t row = i * DIR_ROW_SIZE;
		le::writeU32(span, row, subtableOffsets[i]);
		le::writeU16(span, row + 4, dir.firstFileId);
		le::writeU16(span, row + 6, (i == 0) ? u16(m_directories.size()) : dir.parentId);
	}
	std::copy(subtables.begin(), subtables.end(), out.begin() + tableSize);
	return out;
}

std::size_t NitroFs::fileCount() const
{
	std::size_t count = 0;
	for (const FsDirectory& dir : m_directories)
		for (const FsEntry& entry : dir.entries)
			count += entry.isDirectory ? 0 : 1;
	return count;
}

const FsDirectory* NitroFs::directory(u16 id) const
{
	const std::size_t index = std::size_t(id) - FIRST_DIR_ID;
	return index < m_directories.size() ? &m_directories[index] : nullptr;
}

FsDirectory* NitroFs::directory(u16 id)
{
	return const_cast<FsDirectory*>(std::as_const(*this).directory(id));
}

int NitroFs::findDirectory(std::string_view path) const
{
	if (m_directories.empty())
		return -1;

	u16 current = FIRST_DIR_ID;
	for (std::string_view part : splitPath(path))
	{
		const FsDirectory* dir = directory(current);
		if (dir == nullptr)
			return -1;

		const auto it = std::find_if(dir->entries.begin(), dir->entries.end(),
			[&](const FsEntry& e) { return e.isDirectory && namesEqual(e.name, part); });
		if (it == dir->entries.end())
			return -1;
		current = it->id;
	}
	return int(current);
}

int NitroFs::findFile(std::string_view path) const
{
	const std::size_t slash = path.rfind('/');
	const std::string_view dirPath = (slash == std::string_view::npos) ? std::string_view() : path.substr(0, slash);
	const std::string_view name = (slash == std::string_view::npos) ? path : path.substr(slash + 1);
	if (name.empty())
		return -1;

	const int dirId = findDirectory(dirPath);
	if (dirId < 0)
		return -1;

	const FsDirectory* dir = directory(u16(dirId));
	const auto it = std::find_if(dir->entries.begin(), dir->entries.end(),
		[&](const FsEntry& e) { return !e.isDirectory && namesEqual(e.name, name); });
	return it == dir->entries.end() ? -1 : int(it->id);
}

u16 NitroFs::ensureDirectory(std::string_view path)
{
	if (m_directories.empty())
		m_directories.push_back(FsDirectory{});

	u16 current = FIRST_DIR_ID;
	for (std::string_view part : splitPath(path))
	{
		FsDirectory* parent = directory(current);
		const auto existing = std::find_if(parent->entries.begin(), parent->entries.end(),
			[&](const FsEntry& entry) { return entry.isDirectory && namesEqual(entry.name, part); });
		if (existing != parent->entries.end())
		{
			current = existing->id;
			continue;
		}

		if (part.empty() || part.size() > 0x7F)
			throw ncp::exception("Cannot create a NitroFS directory with an invalid name.");
		if (m_directories.size() >= 0x1000)
			throw ncp::exception("Cannot create another NitroFS directory: all 4096 ids are in use.");

		const u16 childId = u16(FIRST_DIR_ID + m_directories.size());
		FsDirectory child;
		child.id = childId;
		child.parentId = current;
		m_directories.push_back(std::move(child));

		FsEntry entry;
		entry.name = std::string(part);
		entry.isDirectory = true;
		entry.id = childId;
		directory(current)->entries.push_back(std::move(entry));
		current = childId;
	}
	return current;
}

void NitroFs::addFile(std::string_view path, u32 fileId)
{
	if (path.empty() || path.front() == '/' || path.back() == '/'
		|| path.find("//") != std::string_view::npos || path.find('\\') != std::string_view::npos)
		throw ncp::exception("Cannot add an invalid NitroFS path.");
	for (std::string_view part : splitPath(path))
		if (part == "." || part == "..")
			throw ncp::exception("Cannot add an invalid NitroFS path.");
	if (fileId > 0xFFFF)
		throw ncp::exception("Cannot add another named NitroFS file: all 65536 ids are in use.");
	if (findFile(path) >= 0)
		throw ncp::exception("Cannot add a NitroFS file whose path already exists.");

	const std::size_t slash = path.rfind('/');
	const std::string_view dirPath = slash == std::string_view::npos ? std::string_view() : path.substr(0, slash);
	const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
	if (name.empty() || name == "." || name == ".." || name.size() > 0x7F)
		throw ncp::exception("Cannot add a NitroFS file with an invalid name.");

	const u16 dirId = ensureDirectory(dirPath);
	FsDirectory* dir = directory(dirId);
	std::size_t count = 0;
	for (const FsEntry& entry : dir->entries)
		count += entry.isDirectory ? 0 : 1;
	if (count == 0)
		dir->firstFileId = u16(fileId);

	nameFile(dirId, std::string(name), u16(fileId));
}

void NitroFs::nameFile(u16 directoryId, const std::string& name, u16 fileId)
{
	FsDirectory* dir = directory(directoryId);
	if (dir == nullptr)
	{
		std::ostringstream oss;
		oss << "No such directory in the file name table: id 0x" << std::hex << directoryId << ".";
		throw ncp::exception(oss.str());
	}

	// File ids inside a subtable are consecutive from firstFileId, so a name
	// can only be appended when the id being named is the one that comes next.
	// Anything else would renumber files the FAT and every reference to them
	// still describe by their old ids.
	std::size_t fileIndex = 0;
	for (const FsEntry& entry : dir->entries)
		fileIndex += entry.isDirectory ? 0 : 1;

	const u32 expected = u32(dir->firstFileId) + u32(fileIndex);
	if (expected != fileId)
	{
		std::ostringstream oss;
		oss << "Cannot name file id " << fileId << " as " << OSTR(name)
		    << ": that directory's next unnamed file is id " << expected << ".";
		throw ncp::exception(oss.str());
	}

	FsEntry entry;
	entry.name = name;
	entry.isDirectory = false;
	entry.id = fileId;
	dir->entries.push_back(std::move(entry));
}

void NitroFs::renameFile(u32 fileId, std::string_view path)
{
	const std::size_t slash = path.rfind('/');
	const std::string_view dirPath = (slash == std::string_view::npos) ? std::string_view() : path.substr(0, slash);
	const std::string_view name = (slash == std::string_view::npos) ? path : path.substr(slash + 1);
	if (name.empty() || name.size() > 0x7F)
		throw ncp::exception("Cannot rename a NitroFS file to an invalid name.");

	// Where the id lives now. Not findFile: the caller has an id, and the whole
	// point is that the name it currently carries is not the wanted one.
	FsDirectory* owner = nullptr;
	FsEntry* current = nullptr;
	for (FsDirectory& dir : m_directories)
	{
		for (FsEntry& entry : dir.entries)
		{
			if (!entry.isDirectory && entry.id == fileId)
			{
				owner = &dir;
				current = &entry;
				break;
			}
		}
		if (owner != nullptr)
			break;
	}

	if (owner == nullptr)
	{
		std::ostringstream oss;
		oss << "No NitroFS file has id " << fileId << ".";
		throw ncp::exception(oss.str());
	}

	const int destinationDir = findDirectory(dirPath);
	if (destinationDir < 0)
	{
		std::ostringstream oss;
		oss << "Cannot rename file id " << fileId << ": no such directory "
		    << OSTR(std::string(dirPath)) << " in the ROM.";
		throw ncp::exception(oss.str());
	}

	if (u16(destinationDir) != owner->id)
	{
		std::ostringstream oss;
		oss << "Cannot rename file id " << fileId << " to " << OSTR(std::string(path))
		    << ": it belongs to " << OSTR(pathOfFile(fileId))
		    << OREASONNL "A file id is fixed inside its own directory's consecutive range, so it can be "
		       "renamed but not moved. Pick an id from the destination directory.";
		throw ncp::exception(oss.str());
	}

	const auto clash = std::find_if(owner->entries.begin(), owner->entries.end(),
		[&](const FsEntry& entry) { return &entry != current && namesEqual(entry.name, name); });
	if (clash != owner->entries.end())
	{
		std::ostringstream oss;
		oss << "Cannot rename file id " << fileId << " to " << OSTR(std::string(path))
		    << ": that name is already taken in the same directory.";
		throw ncp::exception(oss.str());
	}

	current->name = std::string(name);
}

std::vector<std::pair<u32, std::string>> NitroFs::allFiles() const
{
	std::vector<std::pair<u32, std::string>> out;

	for (const FsDirectory& dir : m_directories)
	{
		// Once per directory rather than once per file: pathOf walks back up to
		// the root, so paying it per entry would re-walk the same chain for
		// every file in a directory.
		const std::string prefix = pathOf(dir.id);
		for (const FsEntry& entry : dir.entries)
		{
			if (entry.isDirectory)
				continue;
			out.emplace_back(u32(entry.id), prefix.empty() ? entry.name : prefix + "/" + entry.name);
		}
	}

	std::sort(out.begin(), out.end(),
		[](const auto& left, const auto& right) { return left.first < right.first; });
	return out;
}

std::string NitroFs::pathOfFile(u32 fileId) const
{
	for (const FsDirectory& dir : m_directories)
	{
		for (const FsEntry& entry : dir.entries)
		{
			if (entry.isDirectory || entry.id != fileId)
				continue;
			const std::string parent = pathOf(dir.id);
			return parent.empty() ? entry.name : parent + "/" + entry.name;
		}
	}
	return {};
}

std::string NitroFs::pathOf(u16 directoryId) const
{
	std::vector<std::string> parts;
	u16 current = directoryId;
	while (current != FIRST_DIR_ID)
	{
		const FsDirectory* dir = directory(current);
		if (dir == nullptr)
			return {};

		const FsDirectory* parent = directory(dir->parentId);
		if (parent == nullptr)
			return {};

		const auto it = std::find_if(parent->entries.begin(), parent->entries.end(),
			[&](const FsEntry& e) { return e.isDirectory && e.id == current; });
		if (it == parent->entries.end())
			return {};

		parts.push_back(it->name);
		current = dir->parentId;
	}

	std::string path;
	for (auto it = parts.rbegin(); it != parts.rend(); ++it)
	{
		if (!path.empty())
			path += '/';
		path += *it;
	}
	return path;
}

} // namespace ncp::rom
