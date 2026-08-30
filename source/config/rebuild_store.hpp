#pragma once

// The record one build leaves for the next.
//
// It replaces <backup>/rebuild.bin, which had two problems that only showed up
// as mysterious rebuilds. It wrote raw std::time_t values, whose width and
// signedness differ between toolchains, so a file written by one build of
// NCPatcher could be silently misread by another. And it decided staleness by
// comparing the config file's modification time against a stored one, which a
// git checkout gets wrong in both directions (it can rewrite an mtime without
// changing a byte, or restore an old file with an old mtime) and which stops
// meaning anything at all once a config can include other files or be assembled
// from modules.
//
// Both are replaced by a hash of the *resolved* configuration: what the build
// actually decided, after inheritance, variables and defines. If that is the
// same, the objects on disk were compiled under the same rules.

#include <filesystem>
#include <string>
#include <vector>

#include "../utils/types.hpp"

namespace ncp::config {

class RebuildStore
{
public:
	// Reads the record if there is one. A missing, unreadable or
	// unrecognized-version file leaves every hash empty, which compares unequal
	// to any real one and so asks for a full rebuild, the safe direction.
	void load(const std::filesystem::path& file);
	void save(const std::filesystem::path& file) const;

	[[nodiscard]] bool projectChanged(const std::string& hash) const { return m_projectHash != hash; }
	[[nodiscard]] bool targetChanged(bool arm9, const std::string& hash) const;

	void setProjectHash(std::string hash) { m_projectHash = std::move(hash); }
	void setTargetHash(bool arm9, std::string hash);

	// The overlays the previous build patched, so they can be reloaded from the
	// backup rather than from the already-patched copy in the ROM.
	[[nodiscard]] std::vector<u32>& patchedOverlays(bool arm9);

private:
	struct Target
	{
		std::string configHash;
		std::vector<u32> patchedOverlays;
	};

	[[nodiscard]] const Target& target(bool arm9) const { return arm9 ? m_arm9 : m_arm7; }
	[[nodiscard]] Target& target(bool arm9) { return arm9 ? m_arm9 : m_arm7; }

	std::string m_projectHash;
	Target m_arm7;
	Target m_arm9;
};

} // namespace ncp::config
