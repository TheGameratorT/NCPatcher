#include "rebuild_store.hpp"

#include <fstream>
#include <sstream>

#include "node.hpp"
#include "../system/except.hpp"
#include "../system/log.hpp"

namespace fs = std::filesystem;

namespace ncp::config {

namespace {

// The on-disk schema version, bumped when the meaning of a field changes.
// An unrecognised value is treated as "no record", not as an error: a build
// must never fail because of a cache it could simply rebuild.
constexpr int FORMAT_VERSION = 1;

void writeOverlayList(std::ostream& out, const std::vector<u32>& overlays)
{
	out << '[';
	for (std::size_t i = 0; i < overlays.size(); i++)
		out << (i ? ", " : "") << overlays[i];
	out << ']';
}

} // namespace

bool RebuildStore::targetChanged(bool arm9, const std::string& hash) const
{
	return target(arm9).configHash != hash;
}

void RebuildStore::setTargetHash(bool arm9, std::string hash)
{
	target(arm9).configHash = std::move(hash);
}

std::vector<u32>& RebuildStore::patchedOverlays(bool arm9)
{
	return target(arm9).patchedOverlays;
}

void RebuildStore::load(const fs::path& file)
{
	if (!fs::exists(file))
		return;

	try {
		const cfg::Document doc(file);
		const cfg::Node root = doc.root();
		if (!root.isMap() || root["version"].asU32(0) != FORMAT_VERSION)
			return;

		m_projectHash = root["project"].asString("");

		const cfg::Node targets = root["targets"];
		for (bool arm9 : { false, true })
		{
			const cfg::Node node = targets[arm9 ? "arm9" : "arm7"];
			if (!node.defined() || !node.isMap())
				continue;

			Target& stored = target(arm9);
			stored.configHash = node["config"].asString("");

			const cfg::Node overlays = node["patched-overlays"];
			if (overlays.defined() && overlays.isSequence())
			{
				for (const cfg::Node& id : overlays.items())
					stored.patchedOverlays.push_back(id.asU32());
			}
		}
	} catch (const std::exception& e) {
		// A corrupt record costs one full rebuild, which is strictly better
		// than refusing to build at all.
		Log::out << OWARN << "Ignoring unreadable " << OSTR(file.filename().string())
		         << "." OREASONNL << e.what() << std::endl;
		*this = RebuildStore();
	}
}

void RebuildStore::save(const fs::path& file) const
{
	std::ostringstream oss;
	oss << "{\n";
	oss << "\t\"version\": " << FORMAT_VERSION << ",\n";
	oss << "\t\"project\": \"" << m_projectHash << "\",\n";
	oss << "\t\"targets\": {\n";

	for (bool arm9 : { false, true })
	{
		const Target& stored = target(arm9);
		oss << "\t\t\"" << (arm9 ? "arm9" : "arm7") << "\": {\n";
		oss << "\t\t\t\"config\": \"" << stored.configHash << "\",\n";
		oss << "\t\t\t\"patched-overlays\": ";
		writeOverlayList(oss, stored.patchedOverlays);
		oss << "\n\t\t}" << (arm9 ? "\n" : ",\n");
	}

	oss << "\t}\n}\n";

	std::ofstream output(file, std::ios::binary);
	if (!output.is_open())
		throw ncp::file_error(file, ncp::file_error::write);
	output << oss.str();
	output.close();

	// The binary record this replaces sits in the same directory and is never
	// read again; leaving it there invites someone to trust it.
	std::error_code ignored;
	fs::remove(file.parent_path() / "rebuild.bin", ignored);
}

} // namespace ncp::config
