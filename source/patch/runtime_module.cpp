// The runtime modules of a target: each `rt:<name>` region linked on its own
// into a relocatable image, after the main link it calls into.

#include "patch_maker.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_set>

#include "patch_tracker.hpp"
#include "linker.hpp"
#include "dependency_resolver.hpp"
#include "runtime_image.hpp"

#include "../system/diagnostics.hpp"
#include "../system/log.hpp"
#include "../system/except.hpp"
#include "../system/cache.hpp"
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

	m_linker->linkRuntimeModule(region, base, units, tracker.getPatchInfo(), tracker.getExternSymbols(), scriptPath, elfPath);

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

	if (rt->sh_addralign > runtime::MaxModuleAlign)
	{
		std::ostringstream oss;
		oss << OSTR(name) << " has data aligned to " << rt->sh_addralign << " bytes, more than the "
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
}

} // namespace ncp::patch
