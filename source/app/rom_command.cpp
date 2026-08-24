#include "rom_command.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "../rom/nds_rom.hpp"
#include "../system/except.hpp"
#include "../system/log.hpp"
#include "../system/message.hpp"

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

void reportArtifact(const char* kind, const std::string& name, std::size_t size, const char* action)
{
	msg::Artifact artifact;
	artifact.kind = kind;
	artifact.action = action;
	artifact.name = name;
	artifact.size = static_cast<long long>(size);
	msg::artifact(std::move(artifact));
}

} // namespace

void info(const fs::path& path, const rom::DirLayout& layout)
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

	Log::out << ANSI_bWHITE << path.string() << ANSI_RESET << '\n';
	Log::out << "  title:        " << header.gameTitle() << " [" << header.gameCode()
	         << "] maker " << header.makerCode() << " rev " << int(header.romVersion()) << '\n';
	Log::out << "  unit code:    " << hex(header.unitCode(), 2)
	         << (header.isDsi() ? "  (DSi)" : "  (DS)") << '\n';
	Log::out << "  capacity:     " << humanSize(header.deviceCapacityBytes())
	         << "  (used " << humanSize(header.totalUsedRomSize()) << ")" << '\n';

	for (bool arm9 : { true, false })
	{
		const rom::ArmBinaryInfo arm = header.arm(arm9);
		Log::out << (arm9 ? "  arm9:         " : "  arm7:         ")
		         << "rom " << hex(arm.romOffset) << " size " << hex(arm.size)
		         << " ram " << hex(arm.ramAddress) << " entry " << hex(arm.entryAddress) << '\n';
	}

	for (bool arm9 : { true, false })
	{
		const rom::RomRegion ovt = header.overlayTable(arm9);
		Log::out << (arm9 ? "  arm9 overlays:" : "  arm7 overlays:") << " ";
		if (ovt.size == 0)
			Log::out << "none\n";
		else
			Log::out << (ovt.size / rom::OverlayEntry::SIZE) << " at " << hex(ovt.romOffset) << '\n';
	}

	Log::out << "  fnt:          " << hex(header.fnt().romOffset) << " size " << hex(header.fnt().size) << '\n';
	Log::out << "  fat:          " << hex(header.fat().romOffset) << " size " << hex(header.fat().size)
	         << "  (" << (header.fat().size / rom::FatEntry::SIZE) << " files)" << '\n';

	// A header whose stored checksum does not match its contents is the single
	// most common sign of a ROM some other tool edited without finishing the
	// job, so it is worth saying out loud rather than leaving to be discovered.
	const u16 stored = header.storedChecksum();
	const u16 computed = header.computeChecksum();
	Log::out << "  header crc:   " << hex(stored, 4)
	         << (stored == computed ? "  (ok)" : "  (WRONG, expected " + hex(computed, 4) + ")") << '\n';

	if (!isDirectory)
		Log::out << "  file size:    " << humanSize(nds.bytes().size()) << '\n';

	Log::out << std::flush;
}

std::size_t extract(const fs::path& romFile, const fs::path& directory, const rom::DirLayout& layout)
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

	for (bool arm9 : { true, false })
	{
		emit(layout.armName(arm9), nds.readArm(arm9), "arm");

		const rom::OverlayTable& table = nds.overlayTable(arm9);
		if (nds.header().overlayTable(arm9).size == 0)
			continue;

		emit(layout.ovtName(arm9), table.serialize(), "overlay-table");

		for (const rom::OverlayEntry& entry : table.entries())
		{
			// Written exactly as the ROM stores it, compression and all. The
			// patcher decompresses on load, so an extracted directory that
			// silently decompressed would disagree with its own overlay table.
			emit(layout.overlayPath(arm9, entry.overlayId), nds.readFile(entry.fileId), "overlay");
		}
	}

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
		if (fs::exists(ovtFile))
		{
			table = rom::OverlayTable::parse(readFile(ovtFile));
			reportArtifact("overlay-table", layout.ovtName(arm9), table.byteSize(), "modified");
			nds.setOverlayTable(arm9, table);
			read++;
		}

		for (const rom::OverlayEntry& entry : table.entries())
		{
			const std::string name = layout.overlayPath(arm9, entry.overlayId);
			const fs::path overlayFile = directory / fs::path(name);
			if (!fs::exists(overlayFile))
				continue;
			std::vector<u8> data = readFile(overlayFile);
			reportArtifact("overlay", name, data.size(), "modified");
			nds.setFile(entry.fileId, std::move(data));
			read++;
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
