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
#include <vector>

#include "header.hpp"
#include "overlay_table.hpp"
#include "../utils/types.hpp"

namespace ncp::rom {

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

	// Flushes whatever the backend has been holding. Backends that write as
	// they go implement it as a no-op; see dir_accessor.hpp for why that
	// difference is deliberate.
	virtual void commit() = 0;
};

} // namespace ncp::rom
