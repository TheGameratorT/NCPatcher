#include "backup_store.hpp"

#include <fstream>
#include <sstream>

#include "../system/except.hpp"
#include "../system/log.hpp"

namespace fs = std::filesystem;

namespace ncp::rom {

namespace {

// "overlay9" / "overlay7": both the subdirectory name and the file prefix.
std::string overlayPrefix(bool arm9)
{
	return arm9 ? "overlay9" : "overlay7";
}

} // namespace

BackupStore::BackupStore(fs::path directory)
	: m_directory(std::move(directory))
{}

std::string BackupStore::armKey(bool arm9) { return arm9 ? "arm9.bin" : "arm7.bin"; }
std::string BackupStore::overlayTableKey(bool arm9) { return arm9 ? "arm9ovt.bin" : "arm7ovt.bin"; }

std::string BackupStore::overlayKey(bool arm9, u32 id)
{
	const std::string prefix = overlayPrefix(arm9);
	return prefix + "/" + prefix + "_" + std::to_string(id) + ".bin";
}

BackupStore::Key BackupStore::classify(std::string_view key)
{
	Key result;
	for (bool arm9 : { false, true })
	{
		if (key == armKey(arm9))
		{
			result.kind = Key::Kind::Arm;
			result.arm9 = arm9;
			return result;
		}
		if (key == overlayTableKey(arm9))
		{
			result.kind = Key::Kind::OverlayTable;
			result.arm9 = arm9;
			return result;
		}

		const std::string prefix = overlayPrefix(arm9) + "/" + overlayPrefix(arm9) + "_";
		if (key.size() > prefix.size() + 4 && key.compare(0, prefix.size(), prefix) == 0
			&& key.compare(key.size() - 4, 4, ".bin") == 0)
		{
			const std::string_view digits = key.substr(prefix.size(), key.size() - prefix.size() - 4);
			if (!digits.empty() && digits.find_first_not_of("0123456789") == std::string_view::npos)
			{
				result.kind = Key::Kind::Overlay;
				result.arm9 = arm9;
				result.overlayId = u32(std::stoul(std::string(digits)));
				return result;
			}
		}
	}
	return result;
}

void BackupStore::createDirectories(bool arm9) const
{
	for (const fs::path& dir : { m_directory, m_directory / overlayPrefix(arm9) })
	{
		if (fs::exists(dir))
			continue;
		if (!fs::create_directories(dir))
		{
			std::ostringstream oss;
			oss << "Could not create backup directory: " << OSTR(dir.string());
			throw ncp::exception(oss.str());
		}
	}
}

fs::path BackupStore::path(const std::string& key) const
{
	return m_directory / fs::path(key);
}

bool BackupStore::has(const std::string& key) const
{
	return fs::exists(path(key));
}

std::vector<u8> BackupStore::read(const std::string& key) const
{
	const fs::path file = path(key);
	if (!fs::exists(file))
		throw ncp::file_error(file, ncp::file_error::find);

	std::ifstream stream(file, std::ios::binary);
	if (!stream.is_open())
		throw ncp::file_error(file, ncp::file_error::read);

	std::vector<u8> bytes(std::size_t(fs::file_size(file)));
	if (!bytes.empty())
	{
		stream.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
		if (!stream)
			throw ncp::file_error(file, ncp::file_error::read);
	}
	return bytes;
}

void BackupStore::write(const std::string& key, std::span<const u8> data) const
{
	const fs::path file = path(key);
	fs::create_directories(file.parent_path());

	std::ofstream stream(file, std::ios::binary);
	if (!stream.is_open())
		throw ncp::file_error(file, ncp::file_error::write);
	stream.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
	if (!stream)
		throw ncp::file_error(file, ncp::file_error::write);
}

} // namespace ncp::rom
