#pragma once

// The extracted-directory backend: loose binaries in a folder, which is what
// every NCPatcher project has used so far and what NSMB-Editor hands the tool.
//
// It writes each file as soon as it is given, rather than buffering until
// commit(). That is not an oversight, it is the behavior projects already
// depend on. A build that fails part way through leaves the binaries it had
// already patched on disk, and the backup directory is what makes that
// recoverable. The container backend cannot work that way, because a half
// written .nds is not a ROM at all; see nds_accessor.hpp.

#include <filesystem>
#include <string>

#include "accessor.hpp"

namespace ncp::rom {

// Which file is which, inside an extracted ROM directory. Different extractors
// disagree about the names, and a project should be able to say so rather than
// having to rename files to suit the patcher.
struct DirLayout
{
	std::string header = "header.bin";
	std::string arm9 = "arm9.bin";
	std::string arm7 = "arm7.bin";
	std::string arm9Ovt = "arm9ovt.bin";
	std::string arm7Ovt = "arm7ovt.bin";
	std::string overlay9Dir = "overlay9";
	std::string overlay7Dir = "overlay7";

	// `{id}` is the overlay number. `{id:4}` pads it with zeroes to 4 digits,
	// which is how ndstool names them.
	std::string overlay9Name = "overlay9_{id}.bin";
	std::string overlay7Name = "overlay7_{id}.bin";

	// Only a full extraction has these. A project directory produced by an
	// editor usually holds the code binaries and nothing else, which is why
	// nothing in the patch path requires them.
	std::string fnt = "fnt.bin";
	std::string fat = "fat.bin";
	std::string banner = "banner.bin";
	std::string dataDir = "data";

	// Named presets, so that a project points at a dump rather than describing
	// it field by field. Returns false for an unknown name.
	static bool preset(std::string_view name, DirLayout& out);
	static std::string presetNames();

	// Overrides one file name on top of a preset, by the name the config uses
	// for it. Returns false for an unknown name, so that a typo is reported
	// rather than silently doing nothing.
	static bool setField(DirLayout& layout, std::string_view key, const std::string& value);
	static std::string fieldNames();

	// Path of an overlay relative to the ROM directory, in generic form.
	[[nodiscard]] std::string overlayPath(bool arm9, u32 id) const;
	[[nodiscard]] const std::string& armName(bool arm9) const { return arm9 ? this->arm9 : this->arm7; }
	[[nodiscard]] const std::string& ovtName(bool arm9) const { return arm9 ? arm9Ovt : arm7Ovt; }
};

class DirRomAccessor final : public RomAccessor
{
public:
	DirRomAccessor(std::filesystem::path directory, DirLayout layout);

	// Reads the header file. Separate from the constructor because a caller may
	// want the accessor built before it is willing to fail on a missing header.
	void loadHeader();

	[[nodiscard]] const std::filesystem::path& location() const override { return m_directory; }
	[[nodiscard]] const DirLayout& layout() const { return m_layout; }

	[[nodiscard]] std::string nameOfArm(bool arm9) const override;
	[[nodiscard]] std::string nameOfOverlayTable(bool arm9) const override;
	[[nodiscard]] std::string nameOfOverlay(bool arm9, u32 id) const override;

	[[nodiscard]] const Header& header() const override { return m_header; }
	[[nodiscard]] Header& header() override { return m_header; }

	[[nodiscard]] std::vector<u8> readArm(bool arm9) override;
	void writeArm(bool arm9, std::span<const u8> data) override;

	[[nodiscard]] OverlayTable readOverlayTable(bool arm9) override;
	void writeOverlayTable(bool arm9, const OverlayTable& table) override;

	[[nodiscard]] bool hasOverlay(bool arm9, u32 id) const override;
	[[nodiscard]] std::vector<u8> readOverlay(bool arm9, u32 id) override;
	void writeOverlay(bool arm9, u32 id, std::span<const u8> data) override;

	u32 createOverlay(bool arm9, u32 id, std::span<const u8> data) override;

	[[nodiscard]] int findNitroFile(std::string_view path) const override;
	[[nodiscard]] std::vector<u8> readNitroFile(std::string_view path) override;
	u32 replaceNitroFile(std::string_view path, std::span<const u8> data) override;
	u32 addNitroFile(std::string_view path, std::span<const u8> data) override;
	void renameNitroFile(u32 fileId, std::string_view path) override;
	[[nodiscard]] std::string nitroFilePath(u32 fileId) const override;

	[[nodiscard]] bool hasBanner() const override;
	[[nodiscard]] std::vector<u8> readBanner() override;
	void writeBanner(std::span<const u8> data) override;

	[[nodiscard]] std::vector<NitroFileInfo> listNitroFiles() const override;

	void commit() override {}

	// Absolute path of a named file inside the ROM directory.
	[[nodiscard]] std::filesystem::path path(const std::string& relative) const;

private:
	std::filesystem::path m_directory;
	DirLayout m_layout;
	Header m_header;
};

} // namespace ncp::rom
