#include "linker_script_generator.hpp"

#include <fstream>
#include <sstream>
#include <algorithm>

#include "../main.hpp"
#include "../log.hpp"
#include "../except.hpp"
#include "../util.hpp"
#include "../process.hpp"
#include "../config/buildconfig.hpp"

namespace fs = std::filesystem;

constexpr std::size_t SizeOfHookBridge = 20;
constexpr std::size_t SizeOfArm2ThumbJumpBridge = 8;

struct PatchType {
    enum {
        Jump, Call, Hook, Over,
        SetJump, SetCall, SetHook,
        RtRepl,
        TJump, TCall, THook,
        SetTJump, SetTCall, SetTHook,
    };
};

LinkerScriptGenerator::LinkerScriptGenerator() = default;
LinkerScriptGenerator::~LinkerScriptGenerator() = default;

void LinkerScriptGenerator::initialize(
    const BuildTarget& target,
    const std::filesystem::path& buildDir,
    const std::vector<std::unique_ptr<SourceFileJob>>& srcFileJobs,
    const std::unordered_map<int, u32>& newcodeAddrForDest
)
{
    m_target = &target;
    m_buildDir = &buildDir;
    m_srcFileJobs = &srcFileJobs;
    m_newcodeAddrForDest = &newcodeAddrForDest;

    m_ldscriptPath = *m_buildDir / (m_target->getArm9() ? "ldscript9.x" : "ldscript7.x");
    m_elfPath = *m_buildDir / (m_target->getArm9() ? "arm9.elf" : "arm7.elf");
}

void LinkerScriptGenerator::createLinkerScript(
    const std::vector<std::unique_ptr<GenericPatchInfo>>& patchInfo,
    const std::vector<std::unique_ptr<RtReplPatchInfo>>& rtreplPatches,
    const std::vector<std::string>& externSymbols,
    const std::vector<int>& destWithNcpSet,
    const std::vector<const SourceFileJob*>& jobsWithNcpSet,
    const std::vector<std::unique_ptr<OverwriteRegionInfo>>& overwriteRegions
)
{
    auto addSectionInclude = [](std::string& o, std::string& objPath, const char* secInc){
        o += "\t\t\"";
        o += objPath;
        o += "\" (.";
        o += secInc;
        o += ")\n";
    };

    auto addSectionPatchInclude = [](std::string& o, GenericPatchInfo*& p) {
        // Convert the section patches into label patches,
        // except for over and set types
        o += "\t\t. = ALIGN(4);\n\t\t";
        o += std::string_view(p->symbol).substr(1);
        o += " = .;\n\t\tKEEP(* (";
        o += p->symbol;
        o += "))\n";
    };

    Log::out << OLINK << "Generating the linker script..." << std::endl;

    fs::current_path(Main::getWorkPath());

    fs::path symbolsFile;
    if (!m_target->symbols.empty())
        symbolsFile = fs::absolute(m_target->symbols);

    std::vector<std::unique_ptr<LDSMemoryEntry>> memoryEntries;
    memoryEntries.emplace_back(new LDSMemoryEntry{ "bin", 0, 0x100000 });

    // Add memory entries for overwrite regions
    for (const auto& overwrite : overwriteRegions)
    {
        if (overwrite->assignedSections.empty())
            continue;
            
        u32 regionSize = overwrite->endAddress - overwrite->startAddress;
        auto* memEntry = new LDSMemoryEntry{ overwrite->memName, overwrite->startAddress, static_cast<int>(regionSize) };
        memoryEntries.emplace_back(memEntry);
    }

    std::vector<std::unique_ptr<LDSRegionEntry>> regionEntries;

    // Overlays must come before arm section
    std::vector<const BuildTarget::Region*> orderedRegions(m_target->regions.size());
    for (std::size_t i = 0; i < m_target->regions.size(); i++)
        orderedRegions[i] = &m_target->regions[i];
    std::sort(orderedRegions.begin(), orderedRegions.end(), [](const BuildTarget::Region* a, const BuildTarget::Region* b){
        return a->destination > b->destination;
    });

    std::vector<int> orderedDestWithNcpSet = destWithNcpSet;
    std::sort(orderedDestWithNcpSet.begin(), orderedDestWithNcpSet.end(), [](int a, int b){
        return a > b;
    });

    for (const BuildTarget::Region* region : orderedRegions)
    {
        LDSMemoryEntry* memEntry;

        int dest = region->destination;
        u32 newcodeAddr = m_newcodeAddrForDest->at(dest);
        if (dest == -1)
        {
            memEntry = new LDSMemoryEntry{ "arm", newcodeAddr, region->length };
        }
        else
        {
            std::string memName; memName.reserve(8);
            memName += "ov";
            memName += std::to_string(dest);
            memEntry = new LDSMemoryEntry{ std::move(memName), newcodeAddr, region->length };
        }

        memoryEntries.emplace_back(memEntry);
        regionEntries.emplace_back(new LDSRegionEntry{ dest, memEntry, region, 0 });
    }

    std::vector<std::unique_ptr<LDSOverPatch>> overPatches;

    // Iterate all patches to setup the linker script
    for (const auto& info : patchInfo)
    {
        if (info->patchType == PatchType::Over)
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
                if (ldsRegion->dest == info->job->region->destination)
                {
                    if (info->sectionIdx != -1)
                    {
                        // Check if this patch's section is assigned to an overwrite region
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

                    if (info->patchType == PatchType::Hook)
                    {
                        ldsRegion->autogenDataSize += SizeOfHookBridge;
                    }
                    else if (info->patchType == PatchType::Jump)
                    {
                        if (!info->destThumb && info->srcThumb) // ARM -> THUMB
                            ldsRegion->autogenDataSize += SizeOfArm2ThumbJumpBridge;
                    }
                }
            }
        }
    }

    if (!orderedDestWithNcpSet.empty())
        memoryEntries.emplace_back(new LDSMemoryEntry{ "ncp_set", 0, 0x100000 });

    std::string o;
    o.reserve(65536);

    o += "/* NCPatcher: Auto-generated linker script */\n\n";

    if (!symbolsFile.empty())
    {
        o += "INCLUDE \"";
        o += Util::relativeIfSubpath(symbolsFile).string();
        o += "\"\n\n";
    }
    
    o += "INPUT (\n";
    for (auto& srcFileJob : *m_srcFileJobs)
    {
        o += "\t\"";
        o += Util::relativeIfSubpath(srcFileJob->objFilePath).string();
        o += "\"\n";
    }

    o += ")\n\nOUTPUT (\"";
    o += Util::relativeIfSubpath(m_elfPath).string();
    o += "\")\n\nMEMORY {\n";

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
        o += overwrite->memName;
        o += " : ALIGN(4) {\n";

        for (auto& p : overwrite->sectionPatches)
            addSectionPatchInclude(o, p);
        
        for (const auto* section : overwrite->assignedSections)
        {
            // Skip ncp_jump, ncp_call, ncp_hook sections as they are already handled above
            if (section->name.starts_with(".ncp_jump") || 
                section->name.starts_with(".ncp_call") || 
                section->name.starts_with(".ncp_hook"))
                continue;

            std::string objPath = Util::relativeIfSubpath(section->job->objFilePath).string();
            o += "\t\t. = ALIGN(";
            o += std::to_string(section->alignment);
            o += ");\n\t\t\"";
            o += objPath;
            o += "\" (";
            o += section->name;
            o += ")\n";
        }
        
        o += "\t\t. = ALIGN(4);\n"
                "\t} > ";
        o += overwrite->memName;
        o += " AT > bin\n\n";
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
            if (p->job->region == s->region)
            {
                std::string_view stem = std::string_view(p->symbol).substr(1);
                o += "\t\t";
                o += stem;
                o += "_start = .;\n\t\t* (";
                o += p->symbol;
                o += ")\n\t\t";
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
            for (auto& f : *m_srcFileJobs)
            {
                if (f->region == s->region)
                {
                    std::string objPath = Util::relativeIfSubpath(f->objFilePath).string();
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
            for (auto& f : *m_srcFileJobs)
            {
                if (f->region == s->region)
                {
                    std::string objPath = Util::relativeIfSubpath(f->objFilePath).string();
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

    for (auto& p : orderedDestWithNcpSet)
    {
        o += "\t.ncp_set";
        if (p == -1)
        {
            o += " : { KEEP(* (.ncp_set)) } > ncp_set AT > bin\n\n";
        }
        else
        {
            o += "_ov";
            o += std::to_string(p);
            o += " : {\n";
            for (auto& j : jobsWithNcpSet)
            {
                if (j->region->destination == p)
                {
                    o += "\t\t KEEP(\"";
                    o += Util::relativeIfSubpath(j->objFilePath).string();
                    o += "\" (.ncp_set))\n\t"
                         "} > ncp_set AT > bin\n\n";
                }
            }
        }
    }

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
    std::ofstream outputFile(m_ldscriptPath);
    if (!outputFile.is_open())
        throw ncp::file_error(m_ldscriptPath, ncp::file_error::write);
    outputFile.write(o.data(), std::streamsize(o.length()));
    outputFile.close();
}

std::string LinkerScriptGenerator::ldFlagsToGccFlags(std::string flags)
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

void LinkerScriptGenerator::linkElfFile()
{
    Log::out << OLINK << "Linking the ARM binary..." << std::endl;

    fs::current_path(Main::getWorkPath());

    std::string ccmd;
    ccmd.reserve(64);
    ccmd += BuildConfig::getToolchain();
    ccmd += "gcc -nostartfiles -Wl,--gc-sections,-T\"";
    ccmd += Util::relativeIfSubpath(m_ldscriptPath).string();
    ccmd += '\"';
    std::string targetFlags = ldFlagsToGccFlags(m_target->ldFlags);
    if (!targetFlags.empty())
        ccmd += ',';
    ccmd += targetFlags;

    std::ostringstream oss;
    int retcode = Process::start(ccmd.c_str(), &oss);
    if (retcode != 0)
    {
        Log::out << oss.str() << std::endl;
        throw ncp::exception("Could not link the ELF file.");
    }
}
