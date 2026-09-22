#include "linker.hpp"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <map>
#include <charconv>

#include "../system/log.hpp"
#include "../system/except.hpp"
#include "../utils/util.hpp"
#include "../utils/unicode.hpp"
#include "../system/process.hpp"

namespace ncp::patch {

constexpr std::size_t SizeOfHookBridge = 20;
constexpr std::size_t SizeOfArm2ThumbJumpBridge = 8;

Linker::Linker() = default;
Linker::~Linker() = default;

void Linker::initialize(
    const BuildTarget& target,
    const ncp::Context& ctx,
    core::CompilationUnitManager& compilationUnitMgr,
    const std::unordered_map<int, u32>& newcodeAddrForDest
)
{
    m_target = &target;
    m_ctx = &ctx;
    m_paths = &ctx.paths;
    m_compilationUnitMgr = &compilationUnitMgr;
    m_newcodeAddrForDest = &newcodeAddrForDest;

    std::string armType = m_target->getArm9() ? "9" : "7";

    m_ldscriptPath = m_paths->buildDir / ("ldscript" + armType + ".x");
    m_elfPath = m_paths->buildDir / ("arm" + armType + ".elf");
    m_measureLdscriptPath = m_paths->buildDir / ("ldscript" + armType + ".measure.x");
    m_measureElfPath = m_paths->buildDir / ("arm" + armType + ".measure.elf");
}

void Linker::createLinkerScript(
    const std::vector<std::unique_ptr<PatchInfo>>& patchInfo,
    const std::vector<std::unique_ptr<PatchInfo>>& rtreplPatches,
    const std::vector<std::string>& externSymbols,
    const std::vector<std::unique_ptr<OverwriteRegionInfo>>& overwriteRegions
)
{
    writeLinkerScript(ScriptMode::Final, patchInfo, rtreplPatches, externSymbols, overwriteRegions, nullptr);
}

void Linker::createMeasurementScript(
    const std::vector<std::unique_ptr<PatchInfo>>& patchInfo,
    const std::vector<std::unique_ptr<PatchInfo>>& rtreplPatches,
    const std::vector<std::string>& externSymbols,
    const std::vector<std::unique_ptr<OverwriteRegionInfo>>& overwriteRegions,
    const std::vector<std::unique_ptr<SectionInfo>>& candidateSections
)
{
    writeLinkerScript(ScriptMode::Measurement, patchInfo, rtreplPatches, externSymbols, overwriteRegions, &candidateSections);
}

void Linker::writeLinkerScript(
    ScriptMode mode,
    const std::vector<std::unique_ptr<PatchInfo>>& patchInfo,
    const std::vector<std::unique_ptr<PatchInfo>>& rtreplPatches,
    const std::vector<std::string>& externSymbols,
    const std::vector<std::unique_ptr<OverwriteRegionInfo>>& overwriteRegions,
    const std::vector<std::unique_ptr<SectionInfo>>* candidateSections
)
{
    auto addSectionInclude = [](std::string& o, std::string& objPath, const char* secInc){
        o += "\t\t\"";
        o += objPath;
        o += "\" (.";
        o += secInc;
        o += ")\n";
    };

    auto addSectionPatchInclude = [](std::string& o, PatchInfo*& p) {
        // Convert the section patches into label patches,
        // except for over and set types
        o += "\t\t. = ALIGN(4);\n\t\t";
        o += std::string_view(p->symbol).substr(1);
        o += " = .;\n\t\tKEEP(* (";
        o += p->symbol;
        o += "))\n";
    };

    // Same naming scheme used for region memory entries below: "arm" for the
    // main binary, "ov<N>" for an overlay.
    auto destMemoryName = [](int dest) {
        std::string memName; memName.reserve(8);
        if (dest == -1) memName = "arm";
        else { memName = "ov"; memName += std::to_string(dest); }
        return memName;
    };

    // Every path written into the linker script is relative to the project dir,
    // because that is the directory the linker itself is run from below.
    std::filesystem::path symbolsFile;
    if (!m_target->symbols.empty())
        symbolsFile = m_paths->work(m_target->symbols);

    std::vector<std::unique_ptr<LDSMemoryEntry>> memoryEntries;
    memoryEntries.emplace_back(new LDSMemoryEntry{ "bin", 0, 0x100000 });

    // Add memory entries for overwrite regions
	for (const auto& overwrite : overwriteRegions)
	{
		if (overwrite->assignedSections.empty())
			continue;
			
		u32 regionSize = overwrite->endAddress - overwrite->startAddress;
		auto* memEntry = new LDSMemoryEntry{ overwrite->name, overwrite->startAddress, static_cast<int>(regionSize) };
		memoryEntries.emplace_back(memEntry);
	}

    // In measurement mode, add one throwaway memory region per destination
    // that has overwrite regions, so every candidate for that destination can
    // be linked (and thus measured) in one pass. The origin reuses the real
    // first region's start address to keep codegen realistic; the length is
    // generous since nothing here is meant to fit anywhere.
    std::map<int, u32> measureOriginForDest;
    if (mode == ScriptMode::Measurement)
    {
        for (const auto& overwrite : overwriteRegions)
            measureOriginForDest.try_emplace(overwrite->destination, overwrite->startAddress);

        for (auto& [dest, origin] : measureOriginForDest)
        {
            std::string memName = "measure_" + destMemoryName(dest);
            memoryEntries.emplace_back(new LDSMemoryEntry{ std::move(memName), origin, 0x400000 });
        }
    }

    std::vector<std::unique_ptr<LDSRegionEntry>> regionEntries;

    // Overlays must come before arm section
    std::vector<const BuildTarget::Region*> orderedRegions(m_target->regions.size());
    for (std::size_t i = 0; i < m_target->regions.size(); i++)
        orderedRegions[i] = &m_target->regions[i];
    std::sort(orderedRegions.begin(), orderedRegions.end(), [](const BuildTarget::Region* a, const BuildTarget::Region* b){
        return a->destination > b->destination;
    });

    for (const BuildTarget::Region* region : orderedRegions)
    {
        LDSMemoryEntry* memEntry;

        int dest = region->destination;
        u32 newcodeAddr = m_newcodeAddrForDest->at(dest);
        if (dest == -1)
        {
            memEntry = new LDSMemoryEntry{ "arm", newcodeAddr, region->maxsize };
        }
        else
        {
            std::string memName; memName.reserve(8);
            memName += "ov";
            memName += std::to_string(dest);
            memEntry = new LDSMemoryEntry{ std::move(memName), newcodeAddr, region->maxsize };
        }

        memoryEntries.emplace_back(memEntry);
        regionEntries.emplace_back(new LDSRegionEntry{ dest, memEntry, region, 0 });
    }

    std::vector<std::unique_ptr<LDSOverPatch>> overPatches;

    // Iterate all patches to setup the linker script
    for (const auto& info : patchInfo)
    {
        if (info->type == PatchType::Over)
        {
            std::string memName; memName.reserve(32);
            memName += "over_";
            memName += Util::intToAddr(int(info->destAddress), 8, false);
            if (info->destAddressOv != -1)
            {
                memName += '_';
                memName += std::to_string(info->destAddressOv);
            }
            auto* memEntry = new LDSMemoryEntry({ std::move(memName), info->destAddress, static_cast<int>(info->sectionSize) });
            memoryEntries.emplace_back(memEntry);
            overPatches.emplace_back(new LDSOverPatch{ info.get(), memEntry });
        }
        else
        {
            for (auto& ldsRegion : regionEntries)
            {
                if (ldsRegion->dest == info->unit->getTargetRegion()->destination)
                {
                    if (info->origin == PatchOrigin::Section && !info->isNcpSet)
                    {
                        // Check if this patch's section is assigned to an overwrite region (only for final version)
                        bool patchInOverwrite = false;

						for (const auto& overwrite : overwriteRegions)
						{
							if (overwrite->destination == ldsRegion->dest)
							{
								for (const auto* section : overwrite->assignedSections)
								{
									if (section->name == info->symbol)
									{
										// Add to sectionPatches of the overwrite region
										overwrite->sectionPatches.emplace_back(info.get());

										patchInOverwrite = true;
										break;
									}
								}
							}
							if (patchInOverwrite) break;
						}
                        
                        // Only add to sectionPatches if not in overwrite region
                        if (!patchInOverwrite)
                            ldsRegion->sectionPatches.emplace_back(info.get());
                    }

                    if (info->type == PatchType::Hook)
                    {
                        ldsRegion->autogenDataSize += SizeOfHookBridge;
                    }
                    else if (info->type == PatchType::Jump)
                    {
                        if (!info->destThumb && info->srcThumb) // ARM -> THUMB
                            ldsRegion->autogenDataSize += SizeOfArm2ThumbJumpBridge;
                    }
                }
            }
        }
    }

    // Check if we have any ncp_set patches
    bool hasNcpSetPatches = std::any_of(patchInfo.begin(), patchInfo.end(), [](const auto& info) {
        return info->isNcpSet;
    });
    
    if (hasNcpSetPatches)
        memoryEntries.emplace_back(new LDSMemoryEntry{ "ncp_set", 0, 0x100000 });

    std::string o;
    o.reserve(65536);

    o += "/* NCPatcher: Auto-generated linker script */\n\n";

    if (!symbolsFile.empty())
    {
        o += "INCLUDE \"";
        o += ncp::pathToUtf8(Util::relativeIfSubpath(symbolsFile, m_paths->workDir));
        o += "\"\n\n";
    }
    
    o += "INPUT (\n";
    for (const auto* unit : m_compilationUnitMgr->getUserUnits())
    {
        o += "\t\"";
        o += ncp::pathToUtf8(Util::relativeIfSubpath(unit->getObjectPath(), m_paths->workDir));
        o += "\"\n";
    }

    const std::filesystem::path& outputElfPath = (mode == ScriptMode::Final) ? m_elfPath : m_measureElfPath;
    const std::filesystem::path& outputScriptPath = (mode == ScriptMode::Final) ? m_ldscriptPath : m_measureLdscriptPath;

    o += ")\n\nOUTPUT (\"";
    o += ncp::pathToUtf8(Util::relativeIfSubpath(outputElfPath, m_paths->workDir));
    o += "\")\n\n";
    
    o += "MEMORY {\n";

    for (auto& memoryEntry : memoryEntries)
    {
        o += '\t';
        o += memoryEntry->name;
        o += " (rwx): ORIGIN = ";
        o += Util::intToAddr(int(memoryEntry->origin), 8);
        o += ", LENGTH = ";
        o += Util::intToAddr(int(memoryEntry->length), 8);
        o += '\n';
    }

    o += "}\n\nSECTIONS {\n";

	// Add overwrite sections
	for (const auto& overwrite : overwriteRegions)
	{
		if (overwrite->assignedSections.empty())
			continue;

		o += "\t.";
		o += overwrite->name;
		o += " : ALIGN(4) {\n";

		for (const auto* section : overwrite->assignedSections)
		{
			// A section that is also a patch target still resolves through
			// KEEP with a label, but it must be emitted here, at the
			// position the packer computed, rather than hoisted in a
			// separate loop ahead of this one: doing both meant the input
			// section was claimed twice, and the linker's first match won
			// at the hoisted (unpacked) spot, leaving this position empty
			// and the region's byte accounting wrong.
			PatchInfo* patch = nullptr;
			for (PatchInfo* p : overwrite->sectionPatches)
			{
				if (p->symbol == section->name)
				{
					patch = p;
					break;
				}
			}

            std::string objPath = ncp::pathToUtf8(Util::relativeIfSubpath(section->unit->getObjectPath(), m_paths->workDir));

			o += "\t\t. = ALIGN(";
			o += std::to_string(section->alignment);
			o += ");\n\t\t";

			if (patch != nullptr)
			{
				o += std::string_view(patch->symbol).substr(1);
				o += " = .;\n\t\tKEEP(\"";
				o += objPath;
				o += "\" (";
				o += section->name;
				o += "))\n";
			}
			else
			{
				o += '"';
				o += objPath;
				o += "\" (";
				o += section->name;
				o += ")\n";
			}
		}

		o += "\t\t. = ALIGN(4);\n"
				"\t} > ";
		o += overwrite->name;
		o += " AT > bin\n\n";
	}

	// In measurement mode, bracket every overwrite candidate for each
	// destination with __ncpm_<idx>_s / __ncpm_<idx>_e symbols, using each
	// candidate's index in the full candidate vector so sizes read back
	// positionally. Emitted before the region blocks so this first-match-wins
	// placement keeps candidates out of the region wildcards below. Patch
	// targets are wrapped in KEEP so they are not trivially collected as
	// live regardless of real usage, which would defeat the measurement.
	if (mode == ScriptMode::Measurement && candidateSections != nullptr)
	{
		for (auto& [dest, origin] : measureOriginForDest)
		{
			o += "\t.measure_";
			o += destMemoryName(dest);
			o += " : ALIGN(4) {\n";

			for (std::size_t idx = 0; idx < candidateSections->size(); idx++)
			{
				const SectionInfo* section = (*candidateSections)[idx].get();
				if (section->unit->getTargetRegion()->destination != dest)
					continue;

				bool isPatchTarget = std::any_of(patchInfo.begin(), patchInfo.end(), [&](const auto& info) {
					return info->origin == PatchOrigin::Section && !info->isNcpSet && info->symbol == section->name;
				});

				std::string objPath = ncp::pathToUtf8(Util::relativeIfSubpath(section->unit->getObjectPath(), m_paths->workDir));

				o += "\t\t. = ALIGN(";
				o += std::to_string(section->alignment);
				o += ");\n\t\t__ncpm_";
				o += std::to_string(idx);
				o += "_s = .;\n\t\t";
				if (isPatchTarget)
				{
					o += "KEEP(\"";
					o += objPath;
					o += "\" (";
					o += section->name;
					o += "))\n";
				}
				else
				{
					o += '"';
					o += objPath;
					o += "\" (";
					o += section->name;
					o += ")\n";
				}
				o += "\t\t__ncpm_";
				o += std::to_string(idx);
				o += "_e = .;\n";
			}

			o += "\t} > measure_";
			o += destMemoryName(dest);
			o += " AT > bin\n\n";
		}
	}

    for (auto& s : regionEntries)
    {
        // TEXT
        o += "\t.";
        o += s->memory->name;
        o += ".text : ALIGN(4) {\n";
        
        for (auto& p : s->sectionPatches)
        {
            addSectionPatchInclude(o, p);
        }
        
        for (auto& p : rtreplPatches)
        {
            if (p->unit->getTargetRegion() == s->region)
            {
                std::string_view stem = std::string_view(p->symbol).substr(1);
                o += "\t\t";
                o += stem;
                o += "_start = .;\n\t\t";
                o += "KEEP(* (";
                o += p->symbol;
                o += "))\n\t\t";
                o += stem;
                o += "_end = .;\n";
            }
        }
        
        if (s->dest == -1)
        {
            o += "\t\t* (.text)\n"
                 "\t\t* (.rodata)\n"
                 "\t\t* (.init_array)\n"
                 "\t\t* (.data)\n"
                 "\t\t* (.text.*)\n"
                 "\t\t* (.rodata.*)\n"
                 "\t\t* (.init_array.*)\n"
                 "\t\t* (.data.*)\n";
            
            if (s->autogenDataSize != 0)
            {
                o += "\t\t. = ALIGN(4);\n"
                     "\t\tncp_autogendata = .;\n"
                     "\t\tFILL(0)\n"
                     "\t\t. = ncp_autogendata + ";
                o += std::to_string(s->autogenDataSize);
                o += ";\n";
            }
        }
        else
        {
    		for (const auto* unit : m_compilationUnitMgr->getUserUnits())
            {
                if (unit->getTargetRegion() == s->region)
                {
                    std::string objPath = ncp::pathToUtf8(Util::relativeIfSubpath(unit->getObjectPath(), m_paths->workDir));
					static const char* secIncs[] = {
						"text",
						"rodata",
						"init_array",
						"data",
						"text.*",
						"rodata.*",
						"init_array.*",
						"data.*"
					};
					for (auto& secInc : secIncs)
						addSectionInclude(o, objPath, secInc);
				}
			}
            
            if (s->autogenDataSize)
            {
                o += "\t\t. = ALIGN(4);\n\t\tncp_autogendata_";
                o += s->memory->name;
                o += " = .;\n\t\tFILL(0)\n\t\t. = ncp_autogendata_";
                o += s->memory->name;
                o += " + ";
                o += std::to_string(s->autogenDataSize);
                o += ";\n";
            }
        }
        o += "\t\t. = ALIGN(4);\n"
             "\t} > ";
        o += s->memory->name;
        o += " AT > bin\n"

        // BSS
             "\n\t.";
        o += s->memory->name;
        o += ".bss : ALIGN(4) {\n";
        if (s->dest == -1)
        {
            o += "\t\t* (.bss)\n"
                 "\t\t* (.bss.*)\n";
        }
        else
        {
    		for (const auto* unit : m_compilationUnitMgr->getUserUnits())
            {
                if (unit->getTargetRegion() == s->region)
                {
                    std::string objPath = ncp::pathToUtf8(Util::relativeIfSubpath(unit->getObjectPath(), m_paths->workDir));
                    addSectionInclude(o, objPath, "bss");
                    addSectionInclude(o, objPath, "bss.*");
                }
            }
        }
        o += "\t\t. = ALIGN(4);\n"
             "\t} > ";
        o += s->memory->name;
        o += " AT > bin\n\n";
    }

    for (auto& p : overPatches)
    {
        o += '\t';
        o += p->info->symbol;
        o += " : { KEEP(* (";
        o += p->info->symbol;
        o += ")) } > ";
        o += p->memory->name;
        o += " AT > bin\n";
    }
    if (!overPatches.empty())
        o += '\n';

	// Generate linker script entries for ncp_set sections
	for (const auto& info : patchInfo)
	{
		if (info->isNcpSet)
		{
			// Create individual section entries for each ncp_set patch
			o += '\t';
			o += info->symbol; // This is the section name like .ncp_setjump_0x02000000
			o += " : { KEEP(\"";
			o += ncp::pathToUtf8(Util::relativeIfSubpath(info->unit->getObjectPath(), m_paths->workDir));
			o += "\" (";
			o += info->symbol;
			o += ")) } > ncp_set AT > bin\n";
		}
	}
	o += '\n';

    o += "\t/DISCARD/ : {*(.*)}\n"
         "}\n";

    if (!externSymbols.empty())
    {
        o += "\nEXTERN (\n";
        for (auto& e : externSymbols)
        {
            o += '\t';
            o += e;
            o += '\n';
        }
        o += ")\n";
    }

    // Output the file
    std::ofstream outputFile(outputScriptPath);
    if (!outputFile.is_open())
        throw ncp::file_error(outputScriptPath, ncp::file_error::write);
    outputFile.write(o.data(), std::streamsize(o.length()));
    outputFile.close();
}

std::string Linker::ldFlagsToGccFlags(std::string flags)
{
    std::size_t cpos = 0;
    while ((cpos = flags.find(' ', cpos)) != std::string::npos)
    {
        std::size_t dpos = flags.find('-', cpos);
        if (dpos == std::string::npos)
            break;
        flags.replace(cpos, dpos - cpos, ",");
        cpos += 2;
    }
    return flags;
}

void Linker::linkElfFile()
{
    std::string ccmd;
    ccmd.reserve(128);
    ccmd += m_ctx->toolchain();
    ccmd += "gcc -nostartfiles -Wl,--gc-sections,-T\"";
    ccmd += ncp::pathToUtf8(Util::relativeIfSubpath(m_ldscriptPath, m_paths->workDir));
    ccmd += '\"';
    std::string targetFlags = ldFlagsToGccFlags(m_target->ldFlags);
    if (!targetFlags.empty())
        ccmd += ',';
    ccmd += targetFlags;

    if (m_ctx->isVerbose(ncp::VerboseTag::Linking))
    {
        Log::out << OINFO << "Linker script: " << ncp::pathToUtf8(m_ldscriptPath) << std::endl;
        Log::out << OINFO << ccmd << std::endl;
    }

    std::ostringstream oss;
    int retcode = Process::start(ccmd.c_str(), m_paths->workDir, &oss);

    if (retcode != 0)
    {
        Log::out << oss.str() << std::endl;
        throw ncp::exception("Could not link the final ELF file.");
    }
}

void Linker::linkMeasurementElf()
{
    std::string ccmd;
    ccmd.reserve(128);
    ccmd += m_ctx->toolchain();
    ccmd += "gcc -nostartfiles -Wl,--gc-sections,-T\"";
    ccmd += ncp::pathToUtf8(Util::relativeIfSubpath(m_measureLdscriptPath, m_paths->workDir));
    ccmd += '\"';
    std::string targetFlags = ldFlagsToGccFlags(m_target->ldFlags);
    if (!targetFlags.empty())
        ccmd += ',';
    ccmd += targetFlags;

    std::ostringstream oss;
    int retcode = Process::start(ccmd.c_str(), m_paths->workDir, &oss);

    if (retcode != 0)
    {
        Log::out << oss.str() << std::endl;
        throw ncp::exception("Could not link the overwrite-region measurement ELF file.");
    }
}

std::vector<u32> Linker::readMeasuredSizes(std::size_t candidateCount)
{
    if (!std::filesystem::exists(m_measureElfPath))
        throw ncp::file_error(m_measureElfPath, ncp::file_error::find);

    Elf32 measureElf;
    if (!measureElf.load(m_measureElfPath))
        throw ncp::file_error(m_measureElfPath, ncp::file_error::read);

    const Elf32_Ehdr& eh = measureElf.getHeader();
    const Elf32_Shdr* sh_tbl = measureElf.getSectionHeaderTable();

    std::vector<u32> starts(candidateCount, 0);
    std::vector<u32> ends(candidateCount, 0);
    std::vector<bool> hasStart(candidateCount, false);
    std::vector<bool> hasEnd(candidateCount, false);

    Elf32::forEachSymbol(measureElf, eh, sh_tbl,
    [&](const Elf32_Sym& symbol, std::string_view symbolName) -> bool {
        constexpr std::string_view prefix = "__ncpm_";
        if (!symbolName.starts_with(prefix))
            return false;

        std::string_view rest = symbolName.substr(prefix.size());
        bool isStart = rest.ends_with("_s");
        bool isEnd = !isStart && rest.ends_with("_e");
        if (!isStart && !isEnd)
            return false;

        rest.remove_suffix(2);

        std::size_t idx;
        auto result = std::from_chars(rest.data(), rest.data() + rest.size(), idx);
        if (result.ec != std::errc() || idx >= candidateCount)
            return false;

        if (isStart) { starts[idx] = symbol.st_value; hasStart[idx] = true; }
        else { ends[idx] = symbol.st_value; hasEnd[idx] = true; }

        return false;
    });

    std::vector<u32> sizes(candidateCount, 0);
    for (std::size_t i = 0; i < candidateCount; i++)
    {
        if (hasStart[i] && hasEnd[i])
            sizes[i] = ends[i] - starts[i];
    }
    return sizes;
}

void Linker::loadElfFile()
{
    if (!std::filesystem::exists(m_elfPath))
        throw ncp::file_error(m_elfPath, ncp::file_error::find);

    m_elf = std::make_unique<Elf32>();
    if (!m_elf->load(m_elfPath))
        throw ncp::file_error(m_elfPath, ncp::file_error::read);
}

void Linker::unloadElfFile()
{
    m_elf = nullptr;
}

} // namespace ncp::patch
