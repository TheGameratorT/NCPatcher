// The runtime modules of a target: each `rt:<name>` region linked on its own
// into a relocatable image, after the main link it calls into.

#include "patch_maker.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_set>

#include "patch_tracker.hpp"
#include "linker.hpp"
#include "dependency_resolver.hpp"
#include "runtime_image.hpp"
#include "../formats/archive.hpp"

#include "../system/diagnostics.hpp"
#include "../system/log.hpp"
#include "../system/except.hpp"
#include "../system/cache.hpp"
#include "../system/message.hpp"
#include "../utils/util.hpp"
#include "../utils/unicode.hpp"

namespace fs = std::filesystem;

namespace ncp::patch {

namespace {

// The symbol a module offset belongs to, as "name+0x10", or the bare offset
// when it precedes every symbol.
std::string describeOffset(const std::vector<std::pair<u32, std::string>>& symbols, u32 offset)
{
	auto it = std::upper_bound(symbols.begin(), symbols.end(), offset,
		[](u32 value, const std::pair<u32, std::string>& symbol) { return value < symbol.first; });

	std::ostringstream oss;
	oss << "offset 0x" << std::hex << std::uppercase << offset;
	if (it != symbols.begin())
	{
		--it;
		oss << " (" << it->second;
		if (offset != it->first)
			oss << "+0x" << (offset - it->first);
		oss << ")";
	}
	return oss.str();
}

void writeBytes(const fs::path& path, const std::vector<u8>& bytes)
{
	if (path.has_parent_path())
		fs::create_directories(path.parent_path());

	std::ofstream file(path, std::ios::binary);
	if (!file.is_open())
		throw ncp::file_error(path, ncp::file_error::write);
	file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
	if (!file)
		throw ncp::file_error(path, ncp::file_error::write);
}

// What the compiler calls to register the destructor of a C++ static object.
// A module's script binds it to the runtime's __ncprt_atexit.
constexpr std::string_view AtexitSymbol = "__aeabi_atexit";

// The places in a module that register a destructor. Each runs at most once
// while the module is attached, so this is the room its list needs.
u32 countAtexitSites(const core::CompilationUnitManager& units)
{
	u32 count = 0;
	for (const auto* unit : units.getUserUnits())
	{
		Elf32* elf = ncp::cache::CacheManager::getInstance().getOrLoadElf(unit->getObjectPath());
		const Elf32_Ehdr& eh = elf->getHeader();
		const Elf32_Shdr* sh_tbl = elf->getSectionHeaderTable();

		for (std::size_t i = 0; i < eh.e_shnum; i++)
		{
			const Elf32_Shdr& rel = sh_tbl[i];
			if (rel.sh_type != SHT_REL || rel.sh_link >= eh.e_shnum)
				continue;

			const Elf32_Shdr& symtab = sh_tbl[rel.sh_link];
			const Elf32_Sym* symbols = elf->getSection<Elf32_Sym>(symtab);
			const std::size_t symbolCount = symtab.sh_size / sizeof(Elf32_Sym);
			const char* names = elf->getSection<char>(sh_tbl[symtab.sh_link]);

			const Elf32_Rel* entries = elf->getSection<Elf32_Rel>(rel);
			for (std::size_t j = 0; j < rel.sh_size / sizeof(Elf32_Rel); j++)
			{
				const std::size_t symbol = ELF32_R_SYM(entries[j].r_info);
				if (symbol != 0 && symbol < symbolCount && AtexitSymbol == &names[symbols[symbol].st_name])
					count++;
			}
		}
	}
	return count;
}

// The largest alignment an input section of the linked module asks for, read
// from the objects and library members the link map lists in .rt.
//
// The section's own alignment says less: ld aligns its long branch veneers to
// 8 bytes, which they do not need, and a module asking for 8 could not be
// loaded from where most games keep files, 4-aligned inside an archive.
u32 inputAlignment(const fs::path& mapPath, const fs::path& workDir)
{
	std::ifstream map(mapPath);
	if (!map.is_open())
		throw ncp::file_error(mapPath, ncp::file_error::read);

	auto& cache = ncp::cache::CacheManager::getInstance();
	std::unordered_map<std::string, std::unique_ptr<Elf32>> members;

	// "lib.a(member.o)" names a member of an archive, anything else a file.
	auto objectOf = [&](const std::string& file) -> const Elf32* {
		const std::size_t open = file.rfind('(');
		if (open == std::string::npos || file.back() != ')')
			return cache.getOrLoadElf(fs::path(file).is_absolute() ? fs::path(file) : workDir / file);

		auto [it, added] = members.try_emplace(file);
		if (added)
		{
			const fs::path archivePath = file.substr(0, open);
			Archive* archive = cache.getOrLoadArchive(archivePath.is_absolute() ? archivePath : workDir / archivePath);
			const ArMember* member = archive ? archive->getMember(file.substr(open + 1, file.size() - open - 2)) : nullptr;
			if (member != nullptr)
			{
				auto elf = std::make_unique<Elf32>();
				if (elf->loadFromMemory(member->data, std::size_t(member->size)))
					it->second = std::move(elf);
			}
		}
		return it->second.get();
	};

	u32 alignment = 4;
	auto account = [&](const std::string& sectionName, const std::string& file) {
		if (file == "linker stubs" || sectionName.ends_with(".__stub"))
			return;

		const Elf32* elf = objectOf(file);
		if (elf == nullptr)
			throw ncp::exception("Could not read " + file + ", which the link map lists, to find its alignment.");

		const Elf32_Ehdr& eh = elf->getHeader();
		const Elf32_Shdr* sh_tbl = elf->getSectionHeaderTable();
		if (sectionName == "COMMON")
		{
			// A common symbol's value is its alignment.
			Elf32::forEachSymbol(*elf, eh, sh_tbl, [&](const Elf32_Sym& symbol, std::string_view) {
				if (symbol.st_shndx == SHN_COMMON)
					alignment = std::max<u32>(alignment, symbol.st_value);
				return false;
			});
			return;
		}

		const char* str_tbl = elf->getSection<char>(sh_tbl[eh.e_shstrndx]);
		Elf32::forEachSection(eh, sh_tbl, str_tbl, [&](std::size_t, const Elf32_Shdr& section, std::string_view name) {
			if (name == sectionName)
				alignment = std::max<u32>(alignment, section.sh_addralign);
			return false;
		});
	};

	// An input section is listed as " name 0xaddress 0xsize file", or with
	// the name alone on its line when it is long and the rest on the next.
	auto isNumber = [](const std::string& token) { return token.starts_with("0x"); };

	bool inImage = false;
	std::string pending;
	std::string line;
	while (std::getline(map, line))
	{
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (line.empty())
			continue;
		if (line[0] != ' ')
		{
			inImage = line.starts_with(".rt");
			pending.clear();
			continue;
		}
		if (!inImage)
			continue;

		std::istringstream iss(line);
		std::vector<std::string> tokens;
		for (std::string token; iss >> token;)
			tokens.push_back(std::move(token));

		auto fileFrom = [&](std::size_t first) {
			std::string file = tokens[first];
			for (std::size_t i = first + 1; i < tokens.size(); i++)
				file += " " + tokens[i];
			return file;
		};

		if (line[1] != ' ')
		{
			pending.clear();
			const std::string& name = tokens[0];
			if (!(name.starts_with(".") || name == "COMMON"))
				continue;
			if (tokens.size() == 1)
				pending = name;
			else if (tokens.size() >= 4 && isNumber(tokens[1]) && isNumber(tokens[2]))
				account(name, fileFrom(3));
		}
		else if (!pending.empty())
		{
			if (tokens.size() >= 3 && isNumber(tokens[0]) && isNumber(tokens[1]))
				account(pending, fileFrom(2));
			pending.clear();
		}
	}
	return alignment;
}

} // namespace

std::vector<std::string> PatchMaker::collectRuntimeImports() const
{
	std::unordered_set<std::string> defined;
	std::vector<std::string> undefined;

	for (const auto& unitMgr : *m_runtimeUnitMgrs)
	{
		for (const auto* unit : unitMgr->getUserUnits())
		{
			Elf32* elf = ncp::cache::CacheManager::getInstance().getOrLoadElf(unit->getObjectPath());
			Elf32::forEachSymbol(*elf, elf->getHeader(), elf->getSectionHeaderTable(),
			[&](const Elf32_Sym& symbol, std::string_view symbolName) {
				if (symbolName.empty() || ELF32_ST_BIND(symbol.st_info) == STB_LOCAL)
					return false;
				if (symbol.st_shndx == SHN_UNDEF)
					undefined.emplace_back(symbolName);
				else
					defined.emplace(symbolName);
				return false;
			});
		}
	}

	std::vector<std::string> imports;
	std::unordered_set<std::string> seen;

	// Bound by each module's own script instead, to its header and to this.
	seen.insert("__dso_handle");
	if (seen.insert(std::string(AtexitSymbol)).second && std::find(undefined.begin(), undefined.end(), AtexitSymbol) != undefined.end())
		undefined.emplace_back("__ncprt_atexit");

	for (std::string& symbolName : undefined)
	{
		if (defined.contains(symbolName) || !seen.insert(symbolName).second)
			continue;
		const DependencyResolver::Symbol* symbol = m_dependencyResolver->findSymbol(symbolName);
		if (symbol != nullptr && (symbol->isGlobal || symbol->isWeak))
			imports.push_back(std::move(symbolName));
	}
	return imports;
}

void PatchMaker::makeRuntimeModules(std::vector<std::unique_ptr<core::CompilationUnitManager>>& runtimeUnitMgrs)
{
	for (std::size_t i = 0; i < m_target->runtimeRegions.size(); i++)
	{
		const BuildTarget::Region& region = m_target->runtimeRegions[i];
		const std::string name = "rt:" + region.runtimeName;

		ncp::ScopedContext ctx(ncp::Diag::PatchRuntimeModule, "Failed to make the runtime module " + name + ".");

		core::CompilationUnitManager& units = *runtimeUnitMgrs[i];
		RuntimeModuleLink a = linkRuntimeModule(region, units, runtime::LinkBaseA);
		RuntimeModuleLink b = linkRuntimeModule(region, units, runtime::LinkBaseB);

		// The comparison below is only meaningful between two links that put
		// everything in the same place.
		if (a.image.size() != b.image.size() || a.symbols != b.symbols)
		{
			std::ostringstream oss;
			oss << OSTR(name) << " does not link to the same layout at 0x" << std::hex << std::uppercase
			    << runtime::LinkBaseA << " and at 0x" << runtime::LinkBaseB << ", so its relocations cannot be found.";
			throw ncp::exception(oss.str());
		}

		std::vector<u32> relocs;
		std::vector<u32> mismatches;
		runtime::findRelocations(a.image, runtime::LinkBaseA, b.image, runtime::LinkBaseB, relocs, mismatches);

		if (!mismatches.empty())
		{
			std::ostringstream oss;
			oss << OSTR(name) << " has " << mismatches.size() << " word" << (mismatches.size() == 1 ? "" : "s")
			    << " that change with its address without being an address in it, which cannot be relocated:";
			constexpr std::size_t MaxListed = 8;
			for (std::size_t j = 0; j < mismatches.size() && j < MaxListed; j++)
				oss << "\n  " << describeOffset(a.symbols, mismatches[j]);
			if (mismatches.size() > MaxListed)
				oss << "\n  and " << (mismatches.size() - MaxListed) << " more";
			throw ncp::exception(oss.str());
		}

		std::vector<u8> image = std::move(a.image);
		const std::size_t imageSize = image.size();
		runtime::appendRelocations(image, relocs);

		std::ostringstream oss;
		oss << name << ", " << Util::humanSize(imageSize - a.bssSize) << " code";
		if (a.bssSize != 0)
			oss << " + " << Util::humanSize(a.bssSize) << " bss";
		oss << ", " << a.patchCount << (a.patchCount == 1 ? " patch" : " patches")
		    << ", " << relocs.size() << (relocs.size() == 1 ? " relocation" : " relocations");
		Log::step("Linking", oss.str());

		writeRuntimeModule(region, image);
	}
}

PatchMaker::RuntimeModuleLink PatchMaker::linkRuntimeModule(const BuildTarget::Region& region, core::CompilationUnitManager& units, u32 base)
{
	const std::string name = "rt:" + region.runtimeName;
	const fs::path dir = m_paths->buildDir / "rt" / region.runtimeName;
	const std::string tag = base == runtime::LinkBaseA ? "a" : "b";
	const fs::path scriptPath = dir / ("ldscript." + tag + ".x");
	const fs::path elfPath = dir / (region.runtimeName + "." + tag + ".elf");
	fs::create_directories(dir);

	for (auto* unit : units.getUserUnits())
		unit->setElf(ncp::cache::CacheManager::getInstance().getOrLoadElf(unit->getObjectPath()));

	// Collected again for each link, since reading the linked ELF rewrites
	// what the tracker holds.
	DependencyResolver resolver;
	resolver.initialize(*m_ctx, units);
	resolver.analyzeObjectFiles();

	PatchTracker tracker;
	tracker.initialize(*m_target, *m_ctx, units, resolver);
	tracker.collectPatchesFromUnits();

	for (const auto& patch : tracker.getPatchInfo())
	{
		if (patch->group.empty())
			continue;
		std::ostringstream oss;
		oss << OSTRa(patch->getPrettyName()) << " (" << OSTR(patch->unit->getSourcePath().string()) << ") is in a patch group, "
		    << "which " << OSTR(name) << " cannot have: the patches of a module are applied when it is loaded.";
		throw ncp::exception(oss.str());
	}

	tracker.checkForOverlappingPatches();

	const u32 atexitCapacity = countAtexitSites(units);
	if (atexitCapacity > 0xFFFF)
		throw ncp::exception(name + " registers the destructors of more than 65535 static objects.");

	m_linker->linkRuntimeModule(region, base, units, tracker.getPatchInfo(), tracker.getExternSymbols(),
		atexitCapacity, scriptPath, elfPath);

	Elf32 elf;
	if (!elf.load(elfPath))
		throw ncp::file_error(elfPath, ncp::file_error::read);

	tracker.finalizePatchesWithElfData(elf);

	const Elf32_Ehdr& eh = elf.getHeader();
	const Elf32_Shdr* sh_tbl = elf.getSectionHeaderTable();
	const char* str_tbl = elf.getSection<char>(sh_tbl[eh.e_shstrndx]);

	const Elf32_Shdr* rt = nullptr;
	Elf32::forEachSection(eh, sh_tbl, str_tbl,
	[&](std::size_t, const Elf32_Shdr& section, std::string_view sectionName) {
		if (sectionName != ".rt")
			return false;
		rt = &section;
		return true;
	});
	if (rt == nullptr || rt->sh_addr != base)
		throw ncp::exception("The linked " + name + " has no image at its base address.");

	const u32 alignment = inputAlignment(Linker::mapPathOf(elfPath), m_paths->workDir);
	if (alignment > runtime::MaxModuleAlign)
	{
		std::ostringstream oss;
		oss << OSTR(name) << " has data aligned to " << alignment << " bytes, more than the "
		    << runtime::MaxModuleAlign << " a module is loaded at.";
		throw ncp::exception(oss.str());
	}

	RuntimeModuleLink link;
	link.image.assign(rt->sh_size, 0);
	if (rt->sh_type != SHT_NOBITS)
		std::memcpy(link.image.data(), elf.getSection<u8>(*rt), rt->sh_size);

	const u32 end = base + rt->sh_size;
	std::unordered_map<std::string, u32> moduleSymbols;
	Elf32::forEachSymbol(elf, eh, sh_tbl,
	[&](const Elf32_Sym& symbol, std::string_view symbolName) {
		if (symbolName.starts_with("__ncprt_module"))
			moduleSymbols.emplace(symbolName, symbol.st_value);

		// ARM's mapping symbols ($a, $t, $d) say nothing a person would
		// recognize, and section symbols have no name.
		u32 address = symbol.st_value & ~1u;
		if (!symbolName.empty() && symbolName[0] != '$' && symbol.st_shndx != SHN_ABS &&
			address >= base && address < end)
		{
			link.symbols.emplace_back(address - base, std::string(symbolName));
		}
		return false;
	});
	std::sort(link.symbols.begin(), link.symbols.end());

	auto symbolAddress = [&](const char* symbolName) {
		auto it = moduleSymbols.find(symbolName);
		if (it == moduleSymbols.end())
			throw ncp::exception("The linked " + name + " has no " + symbolName + " symbol.");
		return it->second;
	};

	// The patches, with the module's hook bridges.
	PatchOperationContext context(
		elf,
		tracker.getPatchInfo(),
		tracker.getNewcodeInfoForDest(),
		tracker.getAutogenDataInfoForDest(),
		m_newcodeAddrForDest,
		sh_tbl
	);

	struct ModuleScope
	{
		PatchMaker& maker;
		ModuleScope(PatchMaker& m, const BuildTarget::Region& r) : maker(m) { maker.m_runtimeModule = &r; maker.m_groupEntries.clear(); }
		~ModuleScope() { maker.m_runtimeModule = nullptr; maker.m_groupEntries.clear(); }
	};

	std::vector<const PatchInfo*> patches;
	for (const auto& patch : tracker.getPatchInfo())
		patches.push_back(patch.get());
	if (patches.size() > 0xFFFF)
		throw ncp::exception(name + " has more than 65535 patches.");

	const u32 tableAddr = symbolAddress("__ncprt_module_patches");
	{
		ModuleScope scope(*this, region);
		applyEachPatch(context);

		auto autogenIt = context.autogenDataInfoForDest->find(BuildTarget::RuntimeDestination);
		if (autogenIt != context.autogenDataInfoForDest->end())
		{
			const AutogenDataInfo& autogen = *autogenIt->second;
			std::memcpy(link.image.data() + (autogen.address - base), autogen.data.data(), autogen.data.size());
		}

		writeGroupEntries(std::span<u8>(link.image).subspan(tableAddr - base), tableAddr, patches);
	}

	const u32 initStart = symbolAddress("__ncprt_module_init");
	const u32 finiStart = symbolAddress("__ncprt_module_fini");
	const u32 exportsStart = symbolAddress("__ncprt_module_exports");

	runtime::ModuleHeader header;
	header.alignLog2 = u8(std::countr_zero(alignment));
	header.base = base;
	header.size = u32(link.image.size());
	header.exports = symbolAddress("__ncprt_module_exports_end") != exportsStart ? exportsStart : 0;
	header.patches = tableAddr;
	header.patchCount = u16(patches.size());
	header.relocs = end;
	header.init = initStart;
	header.initCount = (symbolAddress("__ncprt_module_init_end") - initStart) / 4;
	header.fini = finiStart;
	header.finiCount = (symbolAddress("__ncprt_module_fini_end") - finiStart) / 4;
	header.atexit = symbolAddress("__ncprt_module_atexit");
	header.atexitCapacity = u16(atexitCapacity);
	runtime::writeModuleHeader(link.image, header);

	link.bssSize = symbolAddress("__ncprt_module_end") - symbolAddress("__ncprt_module_bss");
	link.patchCount = patches.size();
	return link;
}

void PatchMaker::writeRuntimeModule(const BuildTarget::Region& region, const std::vector<u8>& image)
{
	const fs::path built = m_paths->buildDir / "rt" / region.runtimeName / (region.runtimeName + ".ncrt");
	writeBytes(built, image);

	for (const fs::path& output : region.outputs)
	{
		writeBytes(output, image);
		Log::FileOnly fileOnly;
		Log::out << OINFO << "Wrote rt:" << region.runtimeName << " to " << ncp::pathToUtf8(output) << std::endl;
	}

	// Over the placeholders the NitroFS insertion made, which keeps their ids.
	for (const std::string& file : region.files)
	{
		const u32 fileId = m_rom->replaceNitroFile(file, image);
		{
			Log::FileOnly fileOnly;
			Log::info("Wrote rt:" + region.runtimeName + " to " + file + " [" + std::to_string(fileId) + "]");
		}

		msg::Artifact artifact;
		artifact.kind = "file";
		artifact.proc = m_target->getArm9() ? "arm9" : "arm7";
		artifact.action = "modified";
		artifact.name = file;
		artifact.size = static_cast<long long>(image.size());
		artifact.fileId = int(fileId);
		msg::artifact(std::move(artifact));
	}
}

} // namespace ncp::patch
