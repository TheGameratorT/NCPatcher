#pragma once

// The container backend: patch a .nds directly.
//
// Unlike the extracted-directory backend it buffers everything until commit().
// It has to: growing the ARM9 binary can move every other region in the ROM, so
// there is no point at which a partially applied set of writes is a ROM anyone
// could boot. Either the whole build lands or the file is left as it was.

#include <filesystem>
#include <optional>

#include "accessor.hpp"
#include "nds_rom.hpp"

namespace ncp::rom {

class NdsRomAccessor final : public RomAccessor
{
public:
	// `output` empty means patch `file` in place.
	NdsRomAccessor(std::filesystem::path file, std::filesystem::path output, u32 arm9Slack);

	void loadRom();

	[[nodiscard]] const std::filesystem::path& location() const override { return m_file; }
	[[nodiscard]] const NdsRom& rom() const { return m_rom; }

	[[nodiscard]] std::string nameOfArm(bool arm9) const override;
	[[nodiscard]] std::string nameOfOverlayTable(bool arm9) const override;
	[[nodiscard]] std::string nameOfOverlay(bool arm9, u32 id) const override;

	[[nodiscard]] const Header& header() const override { return m_rom.header(); }
	[[nodiscard]] Header& header() override { return m_rom.header(); }

	[[nodiscard]] std::vector<u8> readArm(bool arm9) override;
	void writeArm(bool arm9, std::span<const u8> data) override;

	[[nodiscard]] OverlayTable readOverlayTable(bool arm9) override;
	void writeOverlayTable(bool arm9, const OverlayTable& table) override;

	[[nodiscard]] bool hasOverlay(bool arm9, u32 id) const override;
	[[nodiscard]] std::vector<u8> readOverlay(bool arm9, u32 id) override;
	void writeOverlay(bool arm9, u32 id, std::span<const u8> data) override;

	u32 createOverlay(bool arm9, u32 id, std::span<const u8> data) override;

	[[nodiscard]] int findNitroFile(std::string_view path) const override;
	u32 replaceNitroFile(std::string_view path, std::span<const u8> data) override;
	u32 addNitroFile(std::string_view path, std::span<const u8> data) override;

	void commit() override;

private:
	std::filesystem::path m_file;
	std::filesystem::path m_output;
	u32 m_arm9Slack;
	NdsRom m_rom;

	// The table as the build is editing it, per processor. Held here rather
	// than read back from the ROM each time because a write is staged: reading
	// the ROM's copy in between would hand back the unpatched rows.
	std::optional<OverlayTable> m_table[2];

	[[nodiscard]] const OverlayEntry* entry(bool arm9, u32 id) const;
};

} // namespace ncp::rom
