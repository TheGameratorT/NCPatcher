#include "rom_command.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>

#include "../config/node.hpp"
#include "../formats/blz.hpp"
#include "../rom/file_manifest.hpp"
#include "../rom/nds_accessor.hpp"
#include "../rom/nds_rom.hpp"
#include "../system/except.hpp"
#include "../system/log.hpp"
#include "../system/message.hpp"
#include "../utils/json.hpp"

namespace fs = std::filesystem;

namespace ncp::romcmd {

namespace {

std::string hex(u32 value, int width = 8)
{
	std::ostringstream oss;
	oss << "0x" << std::uppercase << std::hex << std::setfill('0') << std::setw(width) << value;
	return oss.str();
}

std::string humanSize(u64 bytes)
{
	std::ostringstream oss;
	if (bytes >= 1024 * 1024)
		oss << (bytes / (1024 * 1024)) << " MiB";
	else if (bytes >= 1024)
		oss << (bytes / 1024) << " KiB";
	else
		oss << bytes << " B";
	return oss.str();
}

void writeFile(const fs::path& path, std::span<const u8> data)
{
	fs::create_directories(path.parent_path());
	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	if (!file.is_open())
		throw ncp::file_error(path, ncp::file_error::write);
	file.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
	if (!file)
		throw ncp::file_error(path, ncp::file_error::write);
}

std::vector<u8> readFile(const fs::path& path)
{
	if (!fs::exists(path))
		throw ncp::file_error(path, ncp::file_error::find);
	std::ifstream file(path, std::ios::binary);
	if (!file.is_open())
		throw ncp::file_error(path, ncp::file_error::read);
	std::vector<u8> bytes;
	bytes.resize(std::size_t(fs::file_size(path)));
	if (!bytes.empty())
	{
		file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
		if (!file)
			throw ncp::file_error(path, ncp::file_error::read);
	}
	return bytes;
}

// What extract wrote about itself, and what pack reads back. Not a schema a
// project ever writes by hand: it exists so that a directory answers the
// questions its own bytes cannot, which is what layout it was written in and
// what form each overlay was stored in.
constexpr const char* MANIFEST_NAME = "extraction.json";

struct OverlayRecord
{
	bool raw = false;         // the extraction wrote it decompressed
	u8 romFlags = 0;          // the flags the ROM's own table had
	u32 romCompressedSize = 0;
};

// [arm7][overlayId], sparse: an overlay with no record is one the manifest did
// not mention, and is left exactly as the directory holds it.
using OverlayRecords = std::map<std::pair<bool, u32>, OverlayRecord>;

void reportArtifact(const char* kind, const std::string& name, std::size_t size, const char* action)
{
	msg::Artifact artifact;
	artifact.kind = kind;
	artifact.action = action;
	artifact.name = name;
	artifact.size = static_cast<long long>(size);
	msg::artifact(std::move(artifact));
}

void writeExtractionManifest(const fs::path& directory,
                             const fs::path& romFile,
                             const rom::DirLayout& layout,
                             const ExtractOptions& options,
                             const OverlayRecords& records)
{
	std::ostringstream out;
	Json::Writer writer(out, 2);

	writer.beginObject();
	writer.field("schema", "ncpatcher.extraction/1");
	writer.field("rom", romFile.filename().string());
	writer.field("code-only", options.codeOnly);
	writer.field("decompressed-overlays", options.decompressOverlays);

	// Every name, so that a tool reading this directory does not have to guess
	// which extractor wrote it. The names are the ones actually used, not a
	// preset's, since a project may have overridden one.
	writer.key("layout").beginObject();
	writer.field("header", layout.header);
	writer.field("arm9", layout.arm9);
	writer.field("arm7", layout.arm7);
	writer.field("arm9-ovt", layout.arm9Ovt);
	writer.field("arm7-ovt", layout.arm7Ovt);
	writer.field("overlay9-dir", layout.overlay9Dir);
	writer.field("overlay7-dir", layout.overlay7Dir);
	writer.field("overlay9-name", layout.overlay9Name);
	writer.field("overlay7-name", layout.overlay7Name);
	writer.field("fnt", layout.fnt);
	writer.field("fat", layout.fat);
	writer.field("banner", layout.banner);
	writer.field("data-dir", layout.dataDir);
	writer.endObject();

	// What the ROM's own table said, whatever the emitted one says. This is the
	// half of the answer the directory cannot carry: once an overlay has been
	// unpacked and its flag cleared, nothing on disk remembers that the ROM had
	// it compressed, and a repack that guessed would be guessing about whether
	// the console can boot.
	writer.key("overlays").beginArray();
	for (const auto& [key, record] : records)
	{
		const auto& [arm9, overlayId] = key;
		writer.beginObject();
		writer.field("proc", arm9 ? "arm9" : "arm7");
		writer.field("id", overlayId);
		writer.field("stored", record.raw ? "raw" : "as-in-rom");
		writer.key("rom-flags").hex(record.romFlags, 2);
		writer.key("rom-compressed-size").value(record.romCompressedSize);
		writer.endObject();
	}
	writer.endArray();
	writer.endObject();
	out << '\n';

	const std::string text = out.str();
	writeFile(directory / MANIFEST_NAME,
		std::span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size()));
	reportArtifact("extraction-manifest", MANIFEST_NAME, text.size(), "created");
}

// Reads back what extract wrote. Absent is not an error: a hand-assembled
// directory, or one from an older extraction, simply has no records and every
// overlay is packed exactly as the directory holds it.
//
// The parser is deliberately small. JSON is a subset of YAML 1.2, so
// cfg::Document reads this without a second reader existing anywhere.
OverlayRecords readExtractionManifest(const fs::path& directory)
{
	OverlayRecords records;

	const fs::path path = directory / MANIFEST_NAME;
	if (!fs::is_regular_file(path))
		return records;

	const cfg::Document document(path);
	const cfg::Node root = document.root();

	const std::string schema = root["schema"].asString("");
	if (schema != "ncpatcher.extraction/1")
	{
		std::ostringstream oss;
		oss << OSTR(path.string()) << " says it is " << OSTR(schema)
		    << ", which this build of NCPatcher does not read.";
		throw ncp::exception(oss.str());
	}

	const cfg::Node overlays = root["overlays"];
	if (!overlays.defined())
		return records;

	for (const cfg::Node& entry : overlays.items())
	{
		const bool arm9 = entry["proc"].asString("arm9") != "arm7";
		const u32 id = entry.require("id").asU32();

		OverlayRecord record;
		record.raw = entry["stored"].asString("as-in-rom") == "raw";
		record.romFlags = u8(entry["rom-flags"].asU32(0));
		record.romCompressedSize = entry["rom-compressed-size"].asU32(0);
		records.emplace(std::pair(arm9, id), record);
	}

	return records;
}

} // namespace

void info(std::ostream& out, const fs::path& path, const rom::DirLayout& layout)
{
	rom::Header header;
	rom::NdsRom nds;
	const bool isDirectory = fs::is_directory(path);

	if (isDirectory)
	{
		header.load(path / fs::path(layout.header));
	}
	else
	{
		nds.load(path);
		header = nds.header();
	}

	out << ANSI_bWHITE << path.string() << ANSI_RESET << '\n';
	out << "  title:        " << header.gameTitle() << " [" << header.gameCode()
	         << "] maker " << header.makerCode() << " rev " << int(header.romVersion()) << '\n';
	out << "  unit code:    " << hex(header.unitCode(), 2)
	         << (header.isDsi() ? "  (DSi)" : "  (DS)") << '\n';
	out << "  capacity:     " << humanSize(header.deviceCapacityBytes())
	         << "  (used " << humanSize(header.totalUsedRomSize()) << ")" << '\n';

	for (bool arm9 : { true, false })
	{
		const rom::ArmBinaryInfo arm = header.arm(arm9);
		out << (arm9 ? "  arm9:         " : "  arm7:         ")
		         << "rom " << hex(arm.romOffset) << " size " << hex(arm.size)
		         << " ram " << hex(arm.ramAddress) << " entry " << hex(arm.entryAddress) << '\n';
	}

	for (bool arm9 : { true, false })
	{
		const rom::RomRegion ovt = header.overlayTable(arm9);
		out << (arm9 ? "  arm9 overlays:" : "  arm7 overlays:") << " ";
		if (ovt.size == 0)
			out << "none\n";
		else
			out << (ovt.size / rom::OverlayEntry::SIZE) << " at " << hex(ovt.romOffset) << '\n';
	}

	out << "  fnt:          " << hex(header.fnt().romOffset) << " size " << hex(header.fnt().size) << '\n';
	out << "  fat:          " << hex(header.fat().romOffset) << " size " << hex(header.fat().size)
	         << "  (" << (header.fat().size / rom::FatEntry::SIZE) << " files)" << '\n';

	// A header whose stored checksum does not match its contents is the single
	// most common sign of a ROM some other tool edited without finishing the
	// job, so it is worth saying out loud rather than leaving to be discovered.
	const u16 stored = header.storedChecksum();
	const u16 computed = header.computeChecksum();
	out << "  header crc:   " << hex(stored, 4)
	         << (stored == computed ? "  (ok)" : "  (WRONG, expected " + hex(computed, 4) + ")") << '\n';

	if (!isDirectory)
		out << "  file size:    " << humanSize(nds.bytes().size()) << '\n';

	out << std::flush;
}

void files(std::ostream& out, const fs::path& path, const rom::DirLayout& layout, bool json)
{
	std::unique_ptr<rom::RomAccessor> accessor;
	if (fs::is_directory(path))
	{
		auto dir = std::make_unique<rom::DirRomAccessor>(path, layout);
		dir->loadHeader();
		accessor = std::move(dir);
	}
	else
	{
		auto nds = std::make_unique<rom::NdsRomAccessor>(path, fs::path(), 0);
		nds->loadRom();
		accessor = std::move(nds);
	}

	std::vector<rom::ManifestEntry> entries;
	for (const rom::NitroFileInfo& file : accessor->listNitroFiles())
	{
		rom::ManifestEntry entry;
		entry.id = file.id;
		entry.path = file.path;
		entry.size = file.size;
		entries.push_back(std::move(entry));
	}
	std::sort(entries.begin(), entries.end(),
		[](const rom::ManifestEntry& left, const rom::ManifestEntry& right) { return left.id < right.id; });

	if (json)
	{
		// No variant: a ROM on disk is the result of a build, not a build.
		rom::writeManifest(out, entries, std::string_view());
		out << std::flush;
		return;
	}

	out << ANSI_bWHITE << path.string() << ANSI_RESET << '\n';
	for (const rom::ManifestEntry& entry : entries)
	{
		out << std::setw(5) << entry.id << "  "
		         << std::setw(9) << entry.size << "  " << entry.path << '\n';
	}
	out << entries.size() << " files\n" << std::flush;
}

std::size_t extract(const fs::path& romFile, const fs::path& directory,
                    const rom::DirLayout& layout, const ExtractOptions& options)
{
	rom::NdsRom nds;
	nds.load(romFile);

	fs::create_directories(directory);

	std::size_t written = 0;
	auto emit = [&](const std::string& name, std::span<const u8> data, const char* kind) {
		writeFile(directory / fs::path(name), data);
		reportArtifact(kind, name, data.size(), "created");
		written++;
	};

	// The whole header *region*, not just the 0x200 bytes of fields: an
	// extracted header.bin has always been the first headerSize bytes, and a
	// shorter one would surprise every tool that already reads these
	// directories. Packing takes the header from the ROM rather than from here,
	// since the patcher never writes it.
	const u32 headerRegion = std::min<u32>(nds.header().headerSize(), u32(nds.bytes().size()));
	emit(layout.header, std::span<const u8>(nds.bytes()).subspan(0, headerRegion), "header");

	OverlayRecords records;

	for (bool arm9 : { true, false })
	{
		emit(layout.armName(arm9), nds.readArm(arm9), "arm");

		if (nds.header().overlayTable(arm9).size == 0)
			continue;

		// A copy, because --decompress-overlays edits the flags of the table it
		// writes out. The ROM's own row is what the manifest records.
		rom::OverlayTable table = nds.overlayTable(arm9);

		for (rom::OverlayEntry& entry : table.entries())
		{
			std::vector<u8> data = nds.readFile(entry.fileId);

			OverlayRecord record;
			record.romFlags = entry.flags;
			record.romCompressedSize = entry.compressedSize;

			// Written exactly as the ROM stores it unless asked otherwise. The
			// patcher decompresses on load, so a directory that silently
			// decompressed would disagree with its own overlay table -- which
			// is why the flag is cleared here in the same breath as the bytes
			// are unpacked, and never one without the other.
			if (options.decompressOverlays && entry.compressed() && !data.empty())
			{
				data = BLZ::uncompress(data);
				entry.setCompressed(false);
				entry.compressedSize = 0;
				record.raw = true;
			}

			records.emplace(std::pair(arm9, entry.overlayId), record);
			emit(layout.overlayPath(arm9, entry.overlayId), data, "overlay");
		}

		emit(layout.ovtName(arm9), table.serialize(), "overlay-table");
	}

	// The rest of the ROM. Without these an extracted directory is half a ROM,
	// and every tool reading one has to know which half: DirRomAccessor has
	// been able to read all four since it was written, and nothing produced
	// them.
	if (!options.codeOnly)
	{
		const rom::RomRegion fnt = nds.header().fnt();
		const rom::RomRegion fat = nds.header().fat();
		if (fnt.size != 0)
		{
			emit(layout.fnt, std::span<const u8>(nds.bytes()).subspan(fnt.romOffset, fnt.size),
				"fnt");
		}
		if (fat.size != 0)
		{
			emit(layout.fat, std::span<const u8>(nds.bytes()).subspan(fat.romOffset, fat.size),
				"fat");
		}
		if (nds.hasBanner())
			emit(layout.banner, nds.readBanner(), "banner");

		const fs::path dataRoot = directory / fs::path(layout.dataDir);
		for (const auto& [id, path] : nds.nitroFs().allFiles())
		{
			writeFile(dataRoot / fs::path(path), nds.readFile(id));
			written++;
		}
	}

	writeExtractionManifest(directory, romFile, layout, options, records);
	written++;

	Log::info("Extracted " + std::to_string(written) + " file(s) to " + directory.string() + ".");
	return written;
}

std::size_t pack(const fs::path& romFile, const fs::path& directory, const fs::path& output,
                 const rom::DirLayout& layout, u32 arm9Slack)
{
	rom::NdsRom nds;
	nds.load(romFile);

	// The ROM is the authority on what exists; the directory only supplies new
	// contents for it. A file the directory does not have is left as the ROM
	// had it, which is what makes packing a directory holding only arm9.bin do
	// what it looks like.
	std::size_t read = 0;
	const OverlayRecords records = readExtractionManifest(directory);

	for (bool arm9 : { true, false })
	{
		const fs::path armFile = directory / fs::path(layout.armName(arm9));
		if (fs::exists(armFile))
		{
			std::vector<u8> data = readFile(armFile);
			reportArtifact("arm", layout.armName(arm9), data.size(), "modified");
			nds.setArm(arm9, std::move(data));
			read++;
		}

		rom::OverlayTable table = nds.overlayTable(arm9);
		const fs::path ovtFile = directory / fs::path(layout.ovtName(arm9));
		const bool haveTable = fs::exists(ovtFile);
		if (haveTable)
		{
			table = rom::OverlayTable::parse(readFile(ovtFile));
			read++;
		}

		bool tableChanged = false;
		for (rom::OverlayEntry& entry : table.entries())
		{
			const std::string name = layout.overlayPath(arm9, entry.overlayId);
			const fs::path overlayFile = directory / fs::path(name);
			if (!fs::exists(overlayFile))
				continue;
			std::vector<u8> data = readFile(overlayFile);

			// An overlay the extraction unpacked goes back in the form the ROM
			// had it, flags and all. Handing the console raw bytes under a row
			// that still says "compressed" is the failure this exists to
			// prevent, so the two move together here exactly as they did on the
			// way out.
			const auto record = records.find(std::pair(arm9, entry.overlayId));
			if (record != records.end() && record->second.raw
				&& (record->second.romFlags & rom::OverlayEntry::FlagCompressed) != 0)
			{
				std::vector<u8> packed = BLZ::compress(data);
				if (packed.empty())
				{
					// It no longer compresses to anything smaller, so it stays
					// raw -- with the flag left clear, which is a ROM the
					// console reads perfectly well.
					Log::warn(name + " no longer compresses smaller than itself; "
						"it is packed uncompressed.");
				}
				else
				{
					entry.flags = record->second.romFlags;
					entry.compressedSize = u32(packed.size());
					data = std::move(packed);
					tableChanged = true;
				}
			}

			reportArtifact("overlay", name, data.size(), "modified");
			nds.setFile(entry.fileId, std::move(data));
			read++;
		}

		if (haveTable || tableChanged)
		{
			reportArtifact("overlay-table", layout.ovtName(arm9), table.byteSize(), "modified");
			nds.setOverlayTable(arm9, table);
		}
	}

	if (read == 0)
	{
		std::ostringstream oss;
		oss << "Nothing to pack: " << OSTR(directory.string()) << " holds none of the files "
		    << "this layout names." << OREASONNL
		    << "Expected at least " << OSTRa(layout.arm9) << " or " << OSTRa(layout.arm9Ovt) << ".";
		throw ncp::exception(oss.str());
	}

	nds.commit(arm9Slack);
	if (nds.lastCommitRebuilt())
		Log::info("The ROM had to be laid out again; every file offset in it has changed.");

	const fs::path destination = output.empty() ? romFile : output;
	nds.save(destination);
	Log::info("Packed " + std::to_string(read) + " file(s) into " + destination.string() + ".");
	return read;
}

} // namespace ncp::romcmd
