#include "plan_accessor.hpp"

#include "../system/except.hpp"

namespace ncp::rom {

namespace {

// Reaching one of these means the caller stopped planning and started
// patching. A thrown message naming the operation is far more useful than a
// silent no-op, which would report a plan that quietly left half the work out.
[[noreturn]] void refuse(const char* what)
{
	throw ncp::exception(std::string("A file plan cannot ") + what + ": it reports what a build "
		"would do and writes nothing.");
}

} // namespace

PlanRomAccessor::PlanRomAccessor(RomAccessor& base)
	: m_base(base)
	, m_tree(base.nitroFs())
	, m_nextId(base.nextNitroFileId())
{
	for (NitroFileInfo& file : m_base.listNitroFiles())
	{
		const u32 id = file.id;
		m_original.emplace(id, std::move(file));
	}
}

void PlanRomAccessor::writeArm(bool, std::span<const u8>) { refuse("write an ARM binary"); }
void PlanRomAccessor::writeOverlayTable(bool, const OverlayTable&) { refuse("write an overlay table"); }
void PlanRomAccessor::writeOverlay(bool, u32, std::span<const u8>) { refuse("write an overlay"); }
u32 PlanRomAccessor::createOverlay(bool, u32, std::span<const u8>) { refuse("create an overlay"); }
void PlanRomAccessor::writeBanner(std::span<const u8>) { refuse("replace the banner"); }

int PlanRomAccessor::findNitroFile(std::string_view path) const
{
	return m_tree.findFile(path);
}

std::vector<u8> PlanRomAccessor::readNitroFile(std::string_view path)
{
	const int fileId = m_tree.findFile(path);
	if (fileId < 0)
		throw ncp::exception("Cannot read a NitroFS path that does not exist: " + std::string(path));

	const auto staged = m_staged.find(u32(fileId));
	if (staged != m_staged.end())
		return staged->second;

	// By id rather than by the path just looked up: a claimed id answers to a
	// name the ROM underneath has never heard of.
	const auto original = m_original.find(u32(fileId));
	if (original == m_original.end())
		throw ncp::exception("Cannot read a planned NitroFS file that has no contents yet.");
	return m_base.readNitroFile(original->second.path);
}

u32 PlanRomAccessor::replaceNitroFile(std::string_view path, std::span<const u8> data)
{
	const int fileId = m_tree.findFile(path);
	if (fileId < 0)
		throw ncp::exception("Cannot replace a NitroFS path that does not exist.");
	m_staged[u32(fileId)].assign(data.begin(), data.end());
	return u32(fileId);
}

u32 PlanRomAccessor::addNitroFile(std::string_view path, std::span<const u8> data)
{
	// addFile first: it is what refuses a destination whose directory cannot
	// take another consecutive id, and a plan that swallowed that would predict
	// an id for a build that is going to stop.
	m_tree.addFile(path, m_nextId);
	const u32 fileId = m_nextId++;
	m_staged[fileId].assign(data.begin(), data.end());
	return fileId;
}

void PlanRomAccessor::renameNitroFile(u32 fileId, std::string_view path)
{
	m_tree.renameFile(fileId, path);
}

std::string PlanRomAccessor::nitroFilePath(u32 fileId) const
{
	return m_tree.pathOfFile(fileId);
}

std::vector<NitroFileInfo> PlanRomAccessor::listNitroFiles() const
{
	std::vector<NitroFileInfo> out;
	for (const auto& [id, path] : m_tree.allFiles())
	{
		NitroFileInfo info;
		info.id = id;
		info.path = path;

		const auto staged = m_staged.find(id);
		if (staged != m_staged.end())
		{
			info.size = u32(staged->second.size());
		}
		else
		{
			const auto original = m_original.find(id);
			if (original != m_original.end())
				info.size = original->second.size;
		}

		out.push_back(std::move(info));
	}
	return out;
}

} // namespace ncp::rom
