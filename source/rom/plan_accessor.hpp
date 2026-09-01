#pragma once

// A ROM accessor that answers everything the real one would and writes nothing,
// anywhere.
//
// `files plan` has to say which file id a `z_new/` addition is going to get,
// and the only trustworthy way to know that is to do what a build does: sweep
// the file trees, classify every destination, and hand the additions to the
// same insertion code, in the same order. Working it out a second way is
// exactly the drift this exists to remove -- an editor that predicts an id the
// build then does not assign is worse than one that admits it does not know.
//
// So the insertion runs for real, against this. Reads fall through to the ROM
// underneath; writes are held here and thrown away once the plan is printed.
// The container backend already buffers until commit(), so for a .nds this
// barely changes anything -- but the extracted-directory backend writes as it
// goes (see dir_accessor.hpp for why), and a command that reports what a build
// *would* do must not patch a byte on the way to saying so.

#include <map>
#include <unordered_map>

#include "accessor.hpp"

namespace ncp::rom {

class PlanRomAccessor final : public RomAccessor
{
public:
	// `base` is borrowed and must outlive this. It is only ever read from.
	explicit PlanRomAccessor(RomAccessor& base);

	[[nodiscard]] const std::filesystem::path& location() const override { return m_base.location(); }

	[[nodiscard]] std::string nameOfArm(bool arm9) const override { return m_base.nameOfArm(arm9); }
	[[nodiscard]] std::string nameOfOverlayTable(bool arm9) const override { return m_base.nameOfOverlayTable(arm9); }
	[[nodiscard]] std::string nameOfOverlay(bool arm9, u32 id) const override { return m_base.nameOfOverlay(arm9, id); }

	[[nodiscard]] const Header& header() const override { return m_base.header(); }
	[[nodiscard]] Header& header() override { return m_base.header(); }

	// The code binaries. A plan is about the file table, so these are reads
	// that pass through and writes that refuse: reaching one would mean the
	// planner had started patching, which is the one thing it must not do.
	[[nodiscard]] std::vector<u8> readArm(bool arm9) override { return m_base.readArm(arm9); }
	void writeArm(bool arm9, std::span<const u8> data) override;

	[[nodiscard]] OverlayTable readOverlayTable(bool arm9) override { return m_base.readOverlayTable(arm9); }
	void writeOverlayTable(bool arm9, const OverlayTable& table) override;

	[[nodiscard]] bool hasOverlay(bool arm9, u32 id) const override { return m_base.hasOverlay(arm9, id); }
	[[nodiscard]] std::vector<u8> readOverlay(bool arm9, u32 id) override { return m_base.readOverlay(arm9, id); }
	void writeOverlay(bool arm9, u32 id, std::span<const u8> data) override;
	u32 createOverlay(bool arm9, u32 id, std::span<const u8> data) override;

	[[nodiscard]] int findNitroFile(std::string_view path) const override;
	[[nodiscard]] std::vector<u8> readNitroFile(std::string_view path) override;
	u32 replaceNitroFile(std::string_view path, std::span<const u8> data) override;
	u32 addNitroFile(std::string_view path, std::span<const u8> data) override;
	void renameNitroFile(u32 fileId, std::string_view path) override;
	[[nodiscard]] std::string nitroFilePath(u32 fileId) const override;

	[[nodiscard]] bool hasBanner() const override { return m_base.hasBanner(); }
	[[nodiscard]] std::vector<u8> readBanner() override { return m_base.readBanner(); }
	void writeBanner(std::span<const u8> data) override;

	[[nodiscard]] std::vector<NitroFileInfo> listNitroFiles() const override;

	[[nodiscard]] NitroFs nitroFs() const override { return m_tree; }
	[[nodiscard]] u32 nextNitroFileId() const override { return m_nextId; }

	// Nothing to flush: that is the point.
	void commit() override {}

private:
	RomAccessor& m_base;

	// The tree as the plan has grown it, starting from the ROM's own.
	NitroFs m_tree;
	u32 m_nextId = 0;

	// Whatever the plan wrote, by file id, and the ROM's own view of a file
	// before it did. The second is needed because a claimed id is renamed
	// before it is replaced, and after the rename the ROM underneath no longer
	// answers to the path the tree now uses.
	std::map<u32, std::vector<u8>> m_staged;
	std::unordered_map<u32, NitroFileInfo> m_original;
};

} // namespace ncp::rom
