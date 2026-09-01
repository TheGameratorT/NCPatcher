#include "file_manifest.hpp"
#include "../utils/unicode.hpp"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include "../utils/json.hpp"

namespace fs = std::filesystem;

namespace ncp::rom {

namespace {

const char* actionName(FileAction action)
{
	switch (action)
	{
	case FileAction::Unchanged: return "unchanged";
	case FileAction::Modified:  return "modified";
	case FileAction::Created:   return "created";
	}
	return "unchanged";
}

// Sources are reported relative to the project when they are inside it, because
// that is how the config wrote them and how a diff will read. One outside stays
// absolute rather than growing a stack of `..` segments.
std::string sourceText(const fs::path& source, const fs::path& projectRoot)
{
	if (source.empty())
		return {};

	std::error_code error;
	const fs::path relative = fs::relative(source, projectRoot, error);

	// The first component, not a prefix of the whole string: `..` is a path
	// element, and a directory genuinely named `..config` is not an escape from
	// the project. Comparing components also sidesteps native() being wide on
	// Windows, where the string comparison would not even compile.
	const bool escapes = !relative.empty() && *relative.begin() == "..";

	if (error || relative.empty() || escapes)
		return pathToUtf8Generic(source);
	return pathToUtf8Generic(relative);
}

} // namespace

std::vector<ManifestEntry> buildManifest(
	const RomAccessor& rom,
	const std::vector<config::FileConfig>& files,
	const std::vector<u32>& createdIds,
	const fs::path& projectRoot,
	const std::vector<std::string>& missingSources)
{
	// Indexed by destination, because that is the only thing the insertion list
	// and the ROM's table share: an entry claiming an id was renamed on the
	// way in, so its path is the one it has now.
	std::unordered_map<std::string, const config::FileConfig*> inserted;
	inserted.reserve(files.size());
	for (const config::FileConfig& file : files)
		inserted.emplace(file.path, &file);

	const std::unordered_set<u32> created(createdIds.begin(), createdIds.end());
	const std::unordered_set<std::string> missing(missingSources.begin(), missingSources.end());

	std::vector<ManifestEntry> out;
	for (const NitroFileInfo& file : rom.listNitroFiles())
	{
		ManifestEntry entry;
		entry.id = file.id;
		entry.path = file.path;
		entry.size = file.size;

		const auto found = inserted.find(file.path);
		if (found != inserted.end())
		{
			entry.action = created.contains(file.id) ? FileAction::Created : FileAction::Modified;
			entry.source = sourceText(found->second->source, projectRoot);
			entry.module = found->second->module;
			entry.component = found->second->component;
			entry.fromVariant = found->second->fromVariant;
			entry.sourceMissing = missing.contains(file.path);
		}
		else if (created.contains(file.id))
		{
			// The reserved placeholder: NCPatcher's own, created out of nothing,
			// so there is no source to name.
			entry.action = FileAction::Created;
		}

		out.push_back(std::move(entry));
	}

	std::sort(out.begin(), out.end(),
		[](const ManifestEntry& left, const ManifestEntry& right) { return left.id < right.id; });
	return out;
}

void writeManifest(std::ostream& out,
                   const std::vector<ManifestEntry>& entries,
                   std::string_view variant,
                   bool planned)
{
	Json::Writer writer(out, 2);

	writer.beginObject();
	writer.field("schema", "ncpatcher.files/1");
	if (!variant.empty())
		writer.field("variant", variant);
	// Only ever written when true, so a document without it is a build's, which
	// is what every existing consumer already assumes it is reading.
	if (planned)
		writer.field("planned", true);
	writer.field("count", entries.size());

	writer.key("files");
	writer.beginArray();
	for (const ManifestEntry& entry : entries)
	{
		writer.beginObject();
		writer.field("id", entry.id);
		writer.field("path", entry.path);
		writer.field("size", entry.size);
		writer.field("action", actionName(entry.action));

		// Only for files this run wrote. Emitting empty strings for the two
		// thousand it did not would double the file to say nothing.
		if (!entry.source.empty())
			writer.field("source", entry.source);
		if (!entry.module.empty())
			writer.field("module", entry.module);
		if (!entry.component.empty())
			writer.field("component", entry.component);
		if (!entry.fromVariant.empty())
			writer.field("from-variant", entry.fromVariant);
		if (entry.sourceMissing)
			writer.field("source-missing", true);
		writer.endObject();
	}
	writer.endArray();
	writer.endObject();
	out << '\n';
}

} // namespace ncp::rom
