#pragma once

// The seam between the patcher and wherever the ROM's bytes actually live.
//
// Everything the patcher needs from a ROM is a small, closed set: the header,
// the two ARM binaries, the two overlay tables, and the overlay files. It never
// reads a level, a texture or a sound. Naming that set explicitly is what makes
// "an extracted directory" and "a .nds file" interchangeable, instead of the
// filesystem calls being spread through the patch code the way they used to be.

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "header.hpp"
#include "overlay_table.hpp"
#include "../utils/types.hpp"

namespace ncp::rom {

// One entry of the ROM's file table, as reported rather than as stored: `size`
// is the file's length in bytes, and `id` is raw -- whatever offset a
// particular game applies to file ids at run time is that game's business.
struct NitroFileInfo
{
	u32 id = 0;
	std::string path;
	u32 size = 0;
};

class RomAccessor
{
public:
	virtual ~RomAccessor() = default;

	// Where the ROM is, for messages. A path either way: a directory for the
	// extracted backend, the .nds file for the container one.
	[[nodiscard]] virtual const std::filesystem::path& location() const = 0;

	// Human-readable name of a binary within the ROM, used in diagnostics and
	// in the machine-readable artifact records. For the extracted backend this
	// is the file's path relative to the ROM directory, which is what tools
	// consuming those records already expect.
	[[nodiscard]] virtual std::string nameOfArm(bool arm9) const = 0;
	[[nodiscard]] virtual std::string nameOfOverlayTable(bool arm9) const = 0;
	[[nodiscard]] virtual std::string nameOfOverlay(bool arm9, u32 id) const = 0;

	[[nodiscard]] virtual const Header& header() const = 0;
	[[nodiscard]] virtual Header& header() = 0;

	[[nodiscard]] virtual std::vector<u8> readArm(bool arm9) = 0;
	virtual void writeArm(bool arm9, std::span<const u8> data) = 0;

	[[nodiscard]] virtual OverlayTable readOverlayTable(bool arm9) = 0;
	virtual void writeOverlayTable(bool arm9, const OverlayTable& table) = 0;

	[[nodiscard]] virtual bool hasOverlay(bool arm9, u32 id) const = 0;
	[[nodiscard]] virtual std::vector<u8> readOverlay(bool arm9, u32 id) = 0;
	virtual void writeOverlay(bool arm9, u32 id, std::span<const u8> data) = 0;

	// Makes room for an overlay that did not exist before and returns the file
	// id it was given. The caller is responsible for the overlay table row; this
	// only settles where the bytes go.
	virtual u32 createOverlay(bool arm9, u32 id, std::span<const u8> data) = 0;

	// Loose NitroFS files, addressed by the same '/'-separated paths the game
	// sees. New paths are deliberately separate from replacement: only z_new/
	// destinations are allowed to call addNitroFile at the application layer.
	[[nodiscard]] virtual int findNitroFile(std::string_view path) const = 0;

	// The file's current contents, with anything this build already staged for
	// it applied. Reading is new with Nitro archives: everything before them
	// only ever wrote whole files, but editing one member of a container means
	// starting from what the container already holds.
	[[nodiscard]] virtual std::vector<u8> readNitroFile(std::string_view path) = 0;

	virtual u32 replaceNitroFile(std::string_view path, std::span<const u8> data) = 0;
	virtual u32 addNitroFile(std::string_view path, std::span<const u8> data) = 0;

	// Renames an existing file id and replaces its data. The third NitroFS
	// operation, and the reason it exists is file-id stability: a project that
	// needs a path the retail ROM never had can repurpose a known-unused id
	// instead of appending one, so nothing after it shifts. The new path's
	// parent directory must be the one already holding the id.
	virtual void renameNitroFile(u32 fileId, std::string_view path) = 0;

	// The path a file id is currently named by, or empty when nothing names it.
	[[nodiscard]] virtual std::string nitroFilePath(u32 fileId) const = 0;

	// The icon/title banner. Not a NitroFS file and not addressable as one: it
	// is a region of its own that the header points at, which is why it needs
	// its own pair of calls rather than a path. A ROM directory that was never
	// fully extracted may not have one at all.
	[[nodiscard]] virtual bool hasBanner() const = 0;
	[[nodiscard]] virtual std::vector<u8> readBanner() = 0;
	virtual void writeBanner(std::span<const u8> data) = 0;

	// Every named NitroFS file the ROM holds, sorted by id.
	//
	// The whole table, not only what this run touched: a consumer generating
	// file-id constants needs the two thousand paths it did not change just as
	// much as the thirteen it did.
	[[nodiscard]] virtual std::vector<NitroFileInfo> listNitroFiles() const = 0;

	// Flushes whatever the backend has been holding. Backends that write as
	// they go implement it as a no-op; see dir_accessor.hpp for why that
	// difference is deliberate.
	virtual void commit() = 0;
};

} // namespace ncp::rom
