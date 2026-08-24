#include "dir_accessor.hpp"

#include <fstream>
#include <sstream>

#include "fat.hpp"
#include "../system/except.hpp"
#include "../system/log.hpp"

namespace fs = std::filesystem;

namespace ncp::rom {

namespace {

std::vector<u8> readWholeFile(const fs::path& path)
{
	if (!fs::exists(path))
		throw ncp::file_error(path, ncp::file_error::find);

	std::ifstream file(path, std::ios::binary);
	if (!file.is_open())
		throw ncp::file_error(path, ncp::file_error::read);

	std::vector<u8> bytes(std::size_t(fs::file_size(path)));
	if (!bytes.empty())
	{
		file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
		if (!file)
			throw ncp::file_error(path, ncp::file_error::read);
	}
	return bytes;
}

void writeWholeFile(const fs::path& path, std::span<const u8> data)
{
	std::ofstream file(path, std::ios::binary);
	if (!file.is_open())
		throw ncp::file_error(path, ncp::file_error::write);
	file.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
	if (!file)
		throw ncp::file_error(path, ncp::file_error::write);
}

// Substitutes `{id}` and `{id:N}` in an overlay file name pattern.
std::string expandOverlayName(const std::string& pattern, u32 id)
{
	std::string out;
	for (std::size_t i = 0; i < pattern.size(); )
	{
		if (pattern.compare(i, 4, "{id}") == 0)
		{
			out += std::to_string(id);
			i += 4;
			continue;
		}
		if (pattern.compare(i, 4, "{id:") == 0)
		{
			const std::size_t close = pattern.find('}', i + 4);
			if (close != std::string::npos)
			{
				const std::string widthText = pattern.substr(i + 4, close - (i + 4));
				std::string digits = std::to_string(id);
				const std::size_t width = widthText.empty() ? 0 : std::size_t(std::stoul(widthText));
				if (digits.size() < width)
					digits.insert(0, width - digits.size(), '0');
				out += digits;
				i = close + 1;
				continue;
			}
		}
		out += pattern[i++];
	}
	return out;
}

} // namespace

bool DirLayout::preset(std::string_view name, DirLayout& out)
{
	if (name == "ncpatcher")
	{
		out = DirLayout{};
		return true;
	}
	if (name == "ndstool")
	{
		// The names `ndstool -x` writes with its documented default arguments.
		DirLayout layout;
		layout.arm9Ovt = "y9.bin";
		layout.arm7Ovt = "y7.bin";
		layout.overlay9Dir = "overlay";
		layout.overlay7Dir = "overlay7";
		layout.overlay9Name = "overlay_{id:4}.bin";
		layout.overlay7Name = "overlay7_{id:4}.bin";
		out = layout;
		return true;
	}
	return false;
}

std::string DirLayout::presetNames()
{
	return "ncpatcher, ndstool";
}

namespace {

// The config's spelling of each field, paired with where it lives. One table
// rather than a chain of comparisons, so that the accepted names and the names
// an error message lists can never drift apart.
struct LayoutField
{
	const char* key;
	std::string DirLayout::* member;
};

constexpr LayoutField LAYOUT_FIELDS[] = {
	{ "header",        &DirLayout::header },
	{ "arm9",          &DirLayout::arm9 },
	{ "arm7",          &DirLayout::arm7 },
	{ "arm9-ovt",      &DirLayout::arm9Ovt },
	{ "arm7-ovt",      &DirLayout::arm7Ovt },
	{ "overlay9-dir",  &DirLayout::overlay9Dir },
	{ "overlay7-dir",  &DirLayout::overlay7Dir },
	{ "overlay9-name", &DirLayout::overlay9Name },
	{ "overlay7-name", &DirLayout::overlay7Name },
	{ "fnt",           &DirLayout::fnt },
	{ "fat",           &DirLayout::fat },
	{ "banner",        &DirLayout::banner },
	{ "data-dir",      &DirLayout::dataDir },
};

} // namespace

bool DirLayout::setField(DirLayout& layout, std::string_view key, const std::string& value)
{
	for (const LayoutField& field : LAYOUT_FIELDS)
	{
		if (key == field.key)
		{
			layout.*(field.member) = value;
			return true;
		}
	}
	return false;
}

std::string DirLayout::fieldNames()
{
	std::string out;
	for (const LayoutField& field : LAYOUT_FIELDS)
	{
		if (!out.empty())
			out += ", ";
		out += field.key;
	}
	return out;
}

std::string DirLayout::overlayPath(bool arm9, u32 id) const
{
	const std::string& dir = arm9 ? overlay9Dir : overlay7Dir;
	const std::string name = expandOverlayName(arm9 ? overlay9Name : overlay7Name, id);
	return dir.empty() ? name : dir + "/" + name;
}

DirRomAccessor::DirRomAccessor(fs::path directory, DirLayout layout)
	: m_directory(std::move(directory)), m_layout(std::move(layout))
{}

fs::path DirRomAccessor::path(const std::string& relative) const
{
	return m_directory / fs::path(relative);
}

void DirRomAccessor::loadHeader()
{
	m_header.load(path(m_layout.header));
}

std::string DirRomAccessor::nameOfArm(bool arm9) const { return m_layout.armName(arm9); }
std::string DirRomAccessor::nameOfOverlayTable(bool arm9) const { return m_layout.ovtName(arm9); }
std::string DirRomAccessor::nameOfOverlay(bool arm9, u32 id) const { return m_layout.overlayPath(arm9, id); }

std::vector<u8> DirRomAccessor::readArm(bool arm9)
{
	return readWholeFile(path(m_layout.armName(arm9)));
}

void DirRomAccessor::writeArm(bool arm9, std::span<const u8> data)
{
	writeWholeFile(path(m_layout.armName(arm9)), data);
}

OverlayTable DirRomAccessor::readOverlayTable(bool arm9)
{
	const fs::path file = path(m_layout.ovtName(arm9));
	if (!fs::exists(file))
	{
		// Most ROMs have no ARM7 overlays, so most extractions have no file to
		// write for that table. The header is what settles whether the absence
		// is expected: it records the table's size, and zero means the ROM
		// genuinely has none rather than the extraction being incomplete.
		if (m_header.loaded() && m_header.overlayTable(arm9).size == 0)
			return {};
		throw ncp::file_error(file, ncp::file_error::find);
	}
	return OverlayTable::parse(readWholeFile(file));
}

void DirRomAccessor::writeOverlayTable(bool arm9, const OverlayTable& table)
{
	// An empty table that was never on disk stays off it. Writing a zero-byte
	// file would be harmless to this tool and confusing to every other one.
	const fs::path file = path(m_layout.ovtName(arm9));
	if (table.empty() && !fs::exists(file))
		return;
	writeWholeFile(file, table.serialize());
}

bool DirRomAccessor::hasOverlay(bool arm9, u32 id) const
{
	return fs::exists(path(m_layout.overlayPath(arm9, id)));
}

std::vector<u8> DirRomAccessor::readOverlay(bool arm9, u32 id)
{
	return readWholeFile(path(m_layout.overlayPath(arm9, id)));
}

void DirRomAccessor::writeOverlay(bool arm9, u32 id, std::span<const u8> data)
{
	const fs::path file = path(m_layout.overlayPath(arm9, id));
	fs::create_directories(file.parent_path());
	writeWholeFile(file, data);
}

u32 DirRomAccessor::createOverlay(bool arm9, u32 id, std::span<const u8> data)
{
	// A new overlay needs a file id, and file ids come from the FAT. A
	// directory that holds only the code binaries -- which is what an editor
	// hands us -- has no FAT to take one from, and inventing an id would
	// produce an overlay the game cannot load.
	const fs::path fatFile = path(m_layout.fat);
	if (!fs::exists(fatFile))
	{
		std::ostringstream oss;
		oss << "Cannot create a new overlay in " << OSTR(m_directory.string()) << OREASONNL
		    << "A new overlay needs a file id from the file allocation table, and "
		    << OSTR(m_layout.fat) << " is not in that directory." << OREASONNL
		    << "Point " << OSTRa("rom.file") << " at the .nds instead, or extract the ROM completely.";
		throw ncp::exception(oss.str());
	}

	Fat fat = Fat::parse(readWholeFile(fatFile));

	// The extent is meaningless in an extracted directory -- the bytes live in
	// their own file -- but the entry has to exist so that the id is taken and
	// a later repack can place it.
	const u32 fileId = fat.add(FatEntry{ 0, u32(data.size()) });
	writeWholeFile(fatFile, fat.serialize());
	writeOverlay(arm9, id, data);
	return fileId;
}

} // namespace ncp::rom
