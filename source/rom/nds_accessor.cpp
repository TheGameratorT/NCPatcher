#include "nds_accessor.hpp"

#include <sstream>

#include "../system/log.hpp"
#include "../system/except.hpp"

namespace fs = std::filesystem;

namespace ncp::rom {

NdsRomAccessor::NdsRomAccessor(fs::path file, fs::path output, u32 arm9Slack)
	: m_file(std::move(file)), m_output(std::move(output)), m_arm9Slack(arm9Slack)
{}

void NdsRomAccessor::loadRom()
{
	m_rom.load(m_file);
}

// The names are the ones the extracted layout uses. A tool reading the
// machine-readable output should not have to care which backend produced it,
// and "arm9.bin" is a better answer to "what was written" than a byte range.
std::string NdsRomAccessor::nameOfArm(bool arm9) const { return arm9 ? "arm9.bin" : "arm7.bin"; }
std::string NdsRomAccessor::nameOfOverlayTable(bool arm9) const { return arm9 ? "arm9ovt.bin" : "arm7ovt.bin"; }

std::string NdsRomAccessor::nameOfOverlay(bool arm9, u32 id) const
{
	const std::string prefix = arm9 ? "overlay9" : "overlay7";
	return prefix + "/" + prefix + "_" + std::to_string(id) + ".bin";
}

std::vector<u8> NdsRomAccessor::readArm(bool arm9)
{
	return m_rom.readArm(arm9);
}

void NdsRomAccessor::writeArm(bool arm9, std::span<const u8> data)
{
	m_rom.setArm(arm9, std::vector<u8>(data.begin(), data.end()));
}

OverlayTable NdsRomAccessor::readOverlayTable(bool arm9)
{
	const int slot = arm9 ? 1 : 0;
	if (!m_table[slot].has_value())
		m_table[slot] = m_rom.overlayTable(arm9);
	return *m_table[slot];
}

void NdsRomAccessor::writeOverlayTable(bool arm9, const OverlayTable& table)
{
	m_table[arm9 ? 1 : 0] = table;
	m_rom.setOverlayTable(arm9, table);
}

const OverlayEntry* NdsRomAccessor::entry(bool arm9, u32 id) const
{
	const int slot = arm9 ? 1 : 0;
	const OverlayTable& table = m_table[slot].has_value() ? *m_table[slot] : m_rom.overlayTable(arm9);
	return id < table.size() ? &table.entries()[id] : nullptr;
}

bool NdsRomAccessor::hasOverlay(bool arm9, u32 id) const
{
	const OverlayEntry* row = entry(arm9, id);
	return row != nullptr && m_rom.hasFile(row->fileId);
}

std::vector<u8> NdsRomAccessor::readOverlay(bool arm9, u32 id)
{
	const OverlayEntry* row = entry(arm9, id);
	if (row == nullptr)
	{
		std::ostringstream oss;
		oss << "The " << (arm9 ? "ARM9" : "ARM7") << " overlay table has no overlay " << id << ".";
		throw ncp::exception(oss.str());
	}

	std::vector<u8> data = m_rom.readFile(row->fileId);

	// The table row's size fields describe the overlay; the FAT extent may be
	// rounded up past them. Trimming keeps a build reading exactly what the
	// game would load.
	const u32 stored = row->storedSize();
	if (stored != 0 && data.size() > stored)
		data.resize(stored);
	return data;
}

void NdsRomAccessor::writeOverlay(bool arm9, u32 id, std::span<const u8> data)
{
	const OverlayEntry* row = entry(arm9, id);
	if (row == nullptr)
	{
		std::ostringstream oss;
		oss << "Cannot write overlay " << id << ": the "
		    << (arm9 ? "ARM9" : "ARM7") << " overlay table has no such entry.";
		throw ncp::exception(oss.str());
	}
	m_rom.setFile(row->fileId, std::vector<u8>(data.begin(), data.end()));
}

u32 NdsRomAccessor::createOverlay(bool /*arm9*/, u32 /*id*/, std::span<const u8> data)
{
	return m_rom.addFile(std::vector<u8>(data.begin(), data.end()));
}

int NdsRomAccessor::findNitroFile(std::string_view path) const
{
	return m_rom.nitroFs().findFile(path);
}

std::vector<u8> NdsRomAccessor::readNitroFile(std::string_view path)
{
	const int fileId = findNitroFile(path);
	if (fileId < 0)
		throw ncp::exception("Cannot read a NitroFS path that does not exist.");
	return m_rom.readFile(u32(fileId));
}

u32 NdsRomAccessor::replaceNitroFile(std::string_view path, std::span<const u8> data)
{
	const int fileId = findNitroFile(path);
	if (fileId < 0)
		throw ncp::exception("Cannot replace a NitroFS path that does not exist.");
	m_rom.setFile(u32(fileId), std::vector<u8>(data.begin(), data.end()));
	return u32(fileId);
}

u32 NdsRomAccessor::addNitroFile(std::string_view path, std::span<const u8> data)
{
	NitroFs tree = m_rom.nitroFs();
	const u32 predictedId = u32(m_rom.fat().size());
	tree.addFile(path, predictedId);

	const u32 fileId = m_rom.addFile(std::vector<u8>(data.begin(), data.end()));
	m_rom.setNitroFs(tree);
	return fileId;
}

bool NdsRomAccessor::hasBanner() const
{
	return m_rom.hasBanner();
}

std::vector<u8> NdsRomAccessor::readBanner()
{
	return m_rom.readBanner();
}

void NdsRomAccessor::writeBanner(std::span<const u8> data)
{
	m_rom.setBanner(std::vector<u8>(data.begin(), data.end()));
}

std::vector<NitroFileInfo> NdsRomAccessor::listNitroFiles() const
{
	std::vector<NitroFileInfo> out;
	for (const auto& [id, path] : m_rom.nitroFs().allFiles())
	{
		NitroFileInfo info;
		info.id = id;
		info.path = path;

		// Staged writes included: the manifest is written before commit(), so
		// asking the FAT would report the size the file had before this build
		// replaced it, and zero for every file it added.
		info.size = m_rom.fileSize(id);
		out.push_back(std::move(info));
	}
	return out;
}

void NdsRomAccessor::renameNitroFile(u32 fileId, std::string_view path)
{
	NitroFs tree = m_rom.nitroFs();
	tree.renameFile(fileId, path);
	m_rom.setNitroFs(tree);
}

std::string NdsRomAccessor::nitroFilePath(u32 fileId) const
{
	return m_rom.nitroFs().pathOfFile(fileId);
}

void NdsRomAccessor::commit()
{
	if (!m_rom.dirty())
	{
		// Nothing was staged, so there is nothing to write, and writing the
		// file anyway would change its timestamp for no reason. An explicit
		// output is the exception: the caller asked for that file to exist.
		if (m_output.empty())
			return;
	}

	m_rom.commit(m_arm9Slack);

	if (m_rom.lastCommitRebuilt())
	{
		Log::info("The ROM had to be laid out again; every file offset in it has changed.");
	}

	const fs::path& destination = m_output.empty() ? m_file : m_output;
	m_rom.save(destination);
	Log::info("Wrote " + destination.string() + ".");
}

} // namespace ncp::rom
