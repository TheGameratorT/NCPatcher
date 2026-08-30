#pragma once

// Pristine copies of every binary the patcher has ever modified.
//
// The patcher is not incremental against the ROM: each build re-applies every
// patch to the *original* binary rather than to whatever last build left
// behind. Without that, an `append` region would grow arm9 on every build and a
// hook would be written over the last build's hook. So the first time a binary
// is touched its unmodified bytes are copied here, and every build afterwards
// reads from here rather than from the ROM.
//
// The file names are fixed rather than taken from the ROM layout. A backup
// directory outlives the configuration that produced it (`ncpatcher restore`
// has to be able to put a directory back years later) and existing projects
// have these names on disk already.

#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "../utils/types.hpp"

namespace ncp::rom {

class BackupStore
{
public:
	explicit BackupStore(std::filesystem::path directory);

	[[nodiscard]] const std::filesystem::path& directory() const { return m_directory; }

	// Keys are paths relative to the backup directory, in generic form. They
	// are also what `restore` copies back into the ROM directory, which is why
	// they mirror the default extracted layout.
	[[nodiscard]] static std::string armKey(bool arm9);
	[[nodiscard]] static std::string overlayTableKey(bool arm9);
	[[nodiscard]] static std::string overlayKey(bool arm9, u32 id);

	// What a key names. `restore` walks the directory rather than being told
	// what is in it, because the set of binaries a project has ever patched is
	// only recorded by the backups themselves.
	struct Key
	{
		enum class Kind { Arm, OverlayTable, Overlay, Unknown };
		Kind kind = Kind::Unknown;
		bool arm9 = false;
		u32 overlayId = 0;
	};

	[[nodiscard]] static Key classify(std::string_view key);

	// Creates the backup directory and the overlay subdirectory for one
	// processor. Called before the first write rather than at startup, so a
	// command that never patches anything does not leave an empty directory.
	void createDirectories(bool arm9) const;

	[[nodiscard]] std::filesystem::path path(const std::string& key) const;
	[[nodiscard]] bool has(const std::string& key) const;
	[[nodiscard]] std::vector<u8> read(const std::string& key) const;
	void write(const std::string& key, std::span<const u8> data) const;

private:
	std::filesystem::path m_directory;
};

} // namespace ncp::rom
