#include "section_usage_analyzer.hpp"

#include <algorithm>
#include <filesystem>

#include "../app/application.hpp"
#include "../system/log.hpp"
#include "../system/except.hpp"
#include "../utils/util.hpp"
#include "../formats/archive.hpp"

SectionUsageAnalyzer::SectionUsageAnalyzer() = default;
SectionUsageAnalyzer::~SectionUsageAnalyzer() = default;

void SectionUsageAnalyzer::initialize(
    const std::vector<std::unique_ptr<GenericPatchInfo>>& patchInfo,
    const std::vector<std::string>& externSymbols,
    core::CompilationUnitManager& compilationUnitMgr
)
{
    m_patchInfo = &patchInfo;
    m_externSymbols = &externSymbols;
    m_compilationUnitMgr = &compilationUnitMgr;
}

void SectionUsageAnalyzer::analyzeObjectFiles()
{
    // Clear previous analysis
    m_sectionUsageInfo.clear();
    m_symbolInfo.clear();
    m_referencedSymbols.clear();
    m_markedSections.clear();
    m_sectionLookup.clear();

    // Phase 1: Collect all symbols and sections from object files
    // Phase 2: Analyze relocations to find dependencies  
    // Combined into single pass to avoid loading ELF files twice
    collectSymbolsAndSectionsWithRelocations();

    // Phase 3: Mark entry points (patches and extern symbols) as used
    markEntryPoints();

    // Phase 4: Propagate usage through dependencies
    propagateUsage();

    if (ncp::Application::isVerbose(ncp::VerboseTag::Section))
    {
        Log::out << OINFO << "Section usage analysis results:" << std::endl;
        Log::out << "  Total sections found: " << m_sectionUsageInfo.size() << std::endl;
        Log::out << "  Sections marked as used: " << m_markedSections.size() << std::endl;
        Log::out << "  Total symbols found: " << m_symbolInfo.size() << std::endl;
        Log::out << "  Symbols marked as referenced: " << m_referencedSymbols.size() << std::endl;
        Log::out << std::endl;
        
        // Print the dependency tree
        // printDependencyTree();
    }
}

// New optimized combined method that loads each ELF file only once
void SectionUsageAnalyzer::collectSymbolsAndSectionsWithRelocations()
{
    for (const auto& unit : m_compilationUnitMgr->getUnits())
    {
        // Try to get ELF directly from the compilation unit first
        Elf32* elf = unit->getElf();

        const Elf32_Ehdr& eh = elf->getHeader();
        auto sh_tbl = elf->getSectionHeaderTable();
        auto str_tbl = elf->getSection<char>(sh_tbl[eh.e_shstrndx]);

        if (ncp::Application::isVerbose(ncp::VerboseTag::Section))
            Log::out << "  Analyzing " << unit->getObjectPath().string() << std::endl;

        // Collect sections
        Elf32::forEachSection(eh, sh_tbl, str_tbl,
        [&](std::size_t sectionIdx, const Elf32_Shdr& section, std::string_view sectionName) -> bool {
            // Skip special sections
            if (sectionName.empty() || sectionName[0] == '\0' ||
                sectionName.starts_with(".rel") || sectionName.starts_with(".debug") ||
                sectionName == ".shstrtab" || sectionName == ".strtab" || 
                sectionName == ".symtab" || section.sh_size == 0)
            {
                return false;
            }

			auto sectionInfo = std::make_unique<SectionUsageInfo>();
			sectionInfo->name = std::string(sectionName);
			sectionInfo->size = section.sh_size;
			sectionInfo->unit = unit.get();
			sectionInfo->alignment = section.sh_addralign > 0 ? section.sh_addralign : 4;
			
			// Add to lookup map immediately for use in relocation analysis
			SectionKey key(sectionInfo->name, sectionInfo->unit);
			SectionUsageInfo* sectionPtr = sectionInfo.get();
			m_sectionLookup[key] = sectionPtr;
			
			m_sectionUsageInfo.push_back(std::move(sectionInfo));
            
            return false;
        });

        // Collect symbols
        Elf32::forEachSymbol(*elf, eh, sh_tbl,
        [&](const Elf32_Sym& symbol, std::string_view symbolName) -> bool {
            if (symbolName.empty() || symbol.st_shndx == SHN_UNDEF)
                return false;

            // Get the section name this symbol belongs to
            std::string sectionName = "";
            if (symbol.st_shndx < eh.e_shnum)
            {
                sectionName = std::string(&str_tbl[sh_tbl[symbol.st_shndx].sh_name]);
            }

            auto symbolInfo = std::make_unique<SymbolInfo>();
            symbolInfo->name = std::string(symbolName);
            symbolInfo->sectionName = sectionName;
            symbolInfo->unit = unit.get();
            symbolInfo->isFunction = (ELF32_ST_TYPE(symbol.st_info) == STT_FUNC);
            symbolInfo->isGlobal = (ELF32_ST_BIND(symbol.st_info) == STB_GLOBAL);
            symbolInfo->isWeak = (ELF32_ST_BIND(symbol.st_info) == STB_WEAK);
            symbolInfo->address = symbol.st_value;

            // Handle weak symbol resolution: strong symbols override weak symbols
            auto existingIt = m_symbolInfo.find(symbolInfo->name);
            if (existingIt != m_symbolInfo.end())
            {
                const SymbolInfo& existing = *existingIt->second;
                
                // If new symbol is strong and existing is weak, replace it
                if (symbolInfo->isGlobal && existing.isWeak)
                {
                    if (ncp::Application::isVerbose(ncp::VerboseTag::Symbols))
                    {
                        Log::out << "    Strong symbol " << symbolInfo->name 
                                 << " overriding weak symbol from " 
                                 << existing.unit->getObjectPath().string() << std::endl;
                    }
                    m_symbolInfo[symbolInfo->name] = std::move(symbolInfo);
                }
                // If new symbol is weak and existing is strong, keep existing
                else if (symbolInfo->isWeak && existing.isGlobal)
                {
                    if (ncp::Application::isVerbose(ncp::VerboseTag::Symbols))
                    {
                        Log::out << "    Weak symbol " << symbolInfo->name 
                                 << " not overriding strong symbol from " 
                                 << existing.unit->getObjectPath().string() << std::endl;
                    }
                    // Don't replace, keep the strong symbol
                }
                // If both are weak, keep the first one (standard linker behavior)
                else if (symbolInfo->isWeak && existing.isWeak)
                {
                    if (ncp::Application::isVerbose(ncp::VerboseTag::Symbols))
                    {
                        Log::out << "    Weak symbol " << symbolInfo->name 
                                 << " not overriding first weak symbol from " 
                                 << existing.unit->getObjectPath().string() << std::endl;
                    }
                    // Don't replace, keep the first weak symbol
                }
                // If both are global, this is a multiple definition error, but we'll keep the first
                else if (symbolInfo->isGlobal && existing.isGlobal)
                {
                    if (ncp::Application::isVerbose(ncp::VerboseTag::Symbols))
                    {
                        Log::out << OWARN << "Multiple definition of global symbol " << symbolInfo->name 
                                 << ": keeping definition from " 
                                 << existing.unit->getObjectPath().string()
                                 << ", ignoring definition from "
                                 << symbolInfo->unit->getObjectPath().string() << std::endl;
                    }
                    // Don't replace, keep the first global symbol
                }
            }
            else
            {
                // First occurrence of this symbol
                m_symbolInfo[symbolInfo->name] = std::move(symbolInfo);
            }
            return false;
        });

        // Analyze relocations in the same pass to avoid loading the ELF again
        Elf32::forEachRelocation(*elf, eh, sh_tbl,
        [&](const Elf32_Rel& relocation, std::string_view relocSectionName, std::string_view targetSectionName) -> bool {
            // Get the symbol table for this relocation section
            const Elf32_Shdr* relocSection = nullptr;
            for (std::size_t i = 0; i < eh.e_shnum; i++)
            {
                if (std::string_view(&str_tbl[sh_tbl[i].sh_name]) == relocSectionName)
                {
                    relocSection = &sh_tbl[i];
                    break;
                }
            }
            
            if (!relocSection || relocSection->sh_link >= eh.e_shnum)
                return false;

            auto sym_tbl = elf->getSection<Elf32_Sym>(sh_tbl[relocSection->sh_link]);
            auto sym_str_tbl = elf->getSection<char>(sh_tbl[sh_tbl[relocSection->sh_link].sh_link]);
            
            u32 symIdx = ELF32_R_SYM(relocation.r_info);
            if (symIdx >= sh_tbl[relocSection->sh_link].sh_size / sizeof(Elf32_Sym))
                return false;

            const Elf32_Sym& referencedSymbol = sym_tbl[symIdx];
            std::string referencedSymbolName;
            bool isSection = false;
            
            if (referencedSymbol.st_name == 0 && ELF32_ST_TYPE(referencedSymbol.st_info) == STT_SECTION)
            {
                // This is a section symbol
                if (referencedSymbol.st_shndx < eh.e_shnum)
                {
                    referencedSymbolName = std::string(&str_tbl[sh_tbl[referencedSymbol.st_shndx].sh_name]);
                    isSection = true;
                }
            }
            else
            {
                // This is a named symbol
                referencedSymbolName = std::string(&sym_str_tbl[referencedSymbol.st_name]);
                isSection = (ELF32_ST_TYPE(referencedSymbol.st_info) == STT_SECTION);
            }

            if (!referencedSymbolName.empty())
            {
                // The targetSectionName is the section that CONTAINS the relocation,
                // not the section being referenced. This section is referencing referencedSymbolName.
                SectionUsageInfo* sectionInfo = findSection(std::string(targetSectionName), unit.get());
                if (sectionInfo)
                {
                    // Create ReferencedSymbol with proper type information
                    ReferencedSymbol refSymbol(referencedSymbolName, isSection);
                    sectionInfo->referencedSymbols.insert(refSymbol);

					// if (ncp::Application::isVerbose())
                    // {
                    //     Log::out << "    Section " << targetSectionName << " references " 
                    //              << (isSection ? "section " : "symbol ") << referencedSymbolName << std::endl;
                    // }
                }
            }
            
            return false;
        });
    }
}

void SectionUsageAnalyzer::markEntryPoints()
{
    // Mark symbols referenced by patches as used
    for (const auto& patchInfo : *m_patchInfo)
    {
        if (patchInfo->sourceType == patch::PatchSourceType::Section) // Label patch
        {
            // For section patches, mark the section itself as an entry point
            SectionUsageInfo* sectionInfo = findSection(patchInfo->symbol, patchInfo->unit);
            if (sectionInfo)
            {
                sectionInfo->isEntryPoint = true;
                markSectionAsUsed(sectionInfo->name, sectionInfo->unit);
            }
        }
    }

    // Mark external symbols as used
    for (const std::string& externSymbol : *m_externSymbols)
    {
        markSymbolAsUsed(externSymbol);
    }
}

void SectionUsageAnalyzer::propagateUsage()
{
    bool changed = true;
    
    while (changed)
    {
        changed = false;
        
        // For each used section, mark all symbols it references as used
        for (const auto& sectionInfo : m_sectionUsageInfo)
        {
            if (m_markedSections.find(sectionInfo.get()) != m_markedSections.end())
            {
                for (const ReferencedSymbol& referencedSymbol : sectionInfo->referencedSymbols)
                {
                    if (referencedSymbol.isSection)
                    {
                        // This is a reference to another section within the same object file.
                        // Section relocations are always internal to the object file - they can
                        // never reference sections from different object files.
                        for (const auto& targetSectionInfo : m_sectionUsageInfo)
                        {
                            if (targetSectionInfo->name == referencedSymbol.name && 
                                targetSectionInfo->unit == sectionInfo->unit &&
                                m_markedSections.find(targetSectionInfo.get()) == m_markedSections.end())
                            {
                                m_markedSections.insert(targetSectionInfo.get());
                                changed = true;
                                break; // Found the section in the same object file
                            }
                        }
                    }
                    else
                    {
                        // This is a reference to a regular symbol
                        if (m_referencedSymbols.find(referencedSymbol.name) == m_referencedSymbols.end())
                        {
                            markSymbolAsUsed(referencedSymbol.name);
                            changed = true;
                        }
                    }
                }
            }
        }
        
        // For each used symbol, mark its containing section as used
        for (const std::string& usedSymbol : m_referencedSymbols)
        {
            // Check user object symbols
            auto it = m_symbolInfo.find(usedSymbol);
            if (it != m_symbolInfo.end())
            {
                const std::string& sectionName = it->second->sectionName;
                if (!sectionName.empty() && !isSectionMarkedAsUsed(sectionName, it->second->unit))
                {
                    markSectionAsUsed(sectionName, it->second->unit);
                    changed = true;
                }
            }
        }
    }
}

void SectionUsageAnalyzer::markSymbolAsUsed(const std::string& symbolName)
{
    m_referencedSymbols.insert(symbolName);
    
    auto it = m_symbolInfo.find(symbolName);
    if (it != m_symbolInfo.end())
    {
        markSectionAsUsed(it->second->sectionName, it->second->unit);
    }
}

void SectionUsageAnalyzer::markSectionAsUsed(const std::string& sectionName, const core::CompilationUnit* unit)
{
    if (!sectionName.empty() && unit != nullptr)
    {
        // Find the specific SectionUsageInfo for this section and unit
        SectionUsageInfo* sectionInfo = findSection(sectionName, unit);
        if (sectionInfo)
        {
            m_markedSections.insert(sectionInfo);
        }
    }
}

bool SectionUsageAnalyzer::isSectionMarkedAsUsed(const std::string& sectionName, const core::CompilationUnit* unit) const
{
    if (sectionName.empty() && unit != nullptr)
        return false;
    
    // Find the specific SectionUsageInfo for this section and unit
    const SectionUsageInfo* sectionInfo = findSection(sectionName, unit);
    if (sectionInfo)
    {
        return m_markedSections.find(const_cast<SectionUsageInfo*>(sectionInfo)) != m_markedSections.end();
    }
    return false;
}

bool SectionUsageAnalyzer::isSymbolInSection(const std::string& symbolName, const std::string& sectionName)
{
    auto it = m_symbolInfo.find(symbolName);
    return (it != m_symbolInfo.end() && it->second->sectionName == sectionName);
}

void SectionUsageAnalyzer::filterUsedSections(std::vector<std::unique_ptr<SectionInfo>>& candidateSections)
{
    auto originalCount = candidateSections.size();
    
    // Remove sections that are not marked as used
    candidateSections.erase(
        std::remove_if(candidateSections.begin(), candidateSections.end(),
            [&](const std::unique_ptr<SectionInfo>& section) -> bool {
                // Find the corresponding SectionUsageInfo for this SectionInfo
                bool isUsed = false;
                for (const auto& sectionUsageInfo : m_sectionUsageInfo)
                {
                    if (sectionUsageInfo->name == section->name && sectionUsageInfo->unit == section->unit)
                    {
                        isUsed = (m_markedSections.find(sectionUsageInfo.get()) != m_markedSections.end());
                        break;
                    }
                }
                
                // if (!isUsed && Main::getVerbose())
                // {
                //     Log::out << OWARN << "Section filtered as unused: " << section->name 
                //              << " from " << section->job->srcFilePath.string() << std::endl;
                // }
                
                return !isUsed;
            }),
        candidateSections.end()
    );

    auto finalCount = candidateSections.size();
    Log::out << OINFO << "Object-level usage analysis complete: " 
             << std::dec << originalCount << " candidate sections -> " 
             << finalCount << " used sections" << std::endl;

    // if (Main::getVerbose() && finalCount > 0)
    // {
    //     Log::out << "Sections available for overwrite assignment:" << std::endl;
    //     for (const auto& section : candidateSections)
    //     {
    //         Log::out << "  " << section->name << " (size: " << section->size 
    //                  << ", align: " << section->alignment << ")" << std::endl;
    //     }
    // }
}

void SectionUsageAnalyzer::printDependencyTree() const
{
    Log::out << OINFO << "Dependency Tree from Entry Points:" << std::endl;
    Log::out << std::endl;
    
    std::unordered_set<std::string> visited;
    bool hasAnyEntryPoints = false;
    
    // Print patch entry points
    for (const auto& patchInfo : *m_patchInfo)
    {
        hasAnyEntryPoints = true;
        std::string patchType = (patchInfo->sectionIdx == -1) ? "Symbol Patch" : "Section Patch";
        Log::out << "📍 " << patchType << ": " << patchInfo->symbol << std::endl;
        
        if (patchInfo->sectionIdx == -1) // Label patch
        {
            printTreeNode(patchInfo->symbol, false, "  ", true, visited);
        }
        else // Section patch
        {
            printTreeNode(patchInfo->symbol, true, "  ", true, visited);
        }
        
        Log::out << std::endl;
        visited.clear(); // Reset visited set for each entry point tree
    }
    
    // Print external symbol entry points
    for (const std::string& externSymbol : *m_externSymbols)
    {
        hasAnyEntryPoints = true;
        Log::out << "🔗 External Symbol: " << externSymbol << std::endl;
        printTreeNode(externSymbol, false, "  ", true, visited);
        Log::out << std::endl;
        visited.clear(); // Reset visited set for each entry point tree
    }
    
    if (!hasAnyEntryPoints)
    {
        Log::out << "  No entry points found." << std::endl;
    }
}

void SectionUsageAnalyzer::printTreeNode(const std::string& nodeName, bool isSection, const std::string& indent, bool isLast, std::unordered_set<std::string>& visited) const
{
    // Avoid infinite loops with circular dependencies
    std::string nodeKey = (isSection ? "sect:" : "sym:") + nodeName;
    if (visited.find(nodeKey) != visited.end())
    {
        Log::out << indent << (isLast ? "└── " : "├── ") << "⚠️  " 
                  << (isSection ? "[Section] " : "") << nodeName << " (circular reference)" << std::endl;
        return;
    }
    visited.insert(nodeKey);
    
    // Print the current node with details
    std::string details = isSection ? getSectionDetails(nodeName) : getSymbolDetails(nodeName);
    std::string symbol = isSection ? "🗂️ " : "⚙️ ";
    Log::out << indent << (isLast ? "└── " : "├── ") << symbol 
             << (isSection ? "[Section] " : "") << nodeName << details << std::endl;
    
    // Find dependencies for this node
    std::vector<std::pair<std::string, bool>> dependencies; // (name, isSection)
    
    if (isSection)
    {
        // Find the section and its dependencies
        for (const auto& sectionInfo : m_sectionUsageInfo)
        {
            if (sectionInfo->name == nodeName)
            {
                for (const ReferencedSymbol& referencedSymbol : sectionInfo->referencedSymbols)
                {
                    // Only include dependencies that are actually used
                    if (referencedSymbol.isSection)
                    {
                        // Check if any section with this name is used
                        for (const auto& candidateSectionInfo : m_sectionUsageInfo)
                        {
                            if (candidateSectionInfo->name == referencedSymbol.name && 
                                m_markedSections.find(candidateSectionInfo.get()) != m_markedSections.end())
                            {
                                dependencies.emplace_back(referencedSymbol.name, true);
                                break; // Only add once per section name
                            }
                        }
                    }
                    else
                    {
                        if (m_referencedSymbols.find(referencedSymbol.name) != m_referencedSymbols.end())
                            dependencies.emplace_back(referencedSymbol.name, false);
                    }
                }
                break;
            }
        }
    }
    else
    {
        // For a symbol, find its containing section
        auto symbolIt = m_symbolInfo.find(nodeName);
        if (symbolIt != m_symbolInfo.end() && !symbolIt->second->sectionName.empty())
        {
            // If this symbol has a section, find that section's dependencies
            for (const auto& sectionInfo : m_sectionUsageInfo)
            {
                if (sectionInfo->name == symbolIt->second->sectionName && sectionInfo->unit == symbolIt->second->unit)
                {
                    for (const ReferencedSymbol& referencedSymbol : sectionInfo->referencedSymbols)
                    {
                        // Only include dependencies that are actually used
                        if (referencedSymbol.isSection)
                        {
                            // Check if any section with this name is used
                            for (const auto& candidateSectionInfo : m_sectionUsageInfo)
                            {
                                if (candidateSectionInfo->name == referencedSymbol.name && 
                                    m_markedSections.find(candidateSectionInfo.get()) != m_markedSections.end())
                                {
                                    dependencies.emplace_back(referencedSymbol.name, true);
                                    break; // Only add once per section name
                                }
                            }
                        }
                        else
                        {
                            if (m_referencedSymbols.find(referencedSymbol.name) != m_referencedSymbols.end())
                                dependencies.emplace_back(referencedSymbol.name, false);
                        }
                    }
                    break;
                }
            }
        }
    }
    
    // Sort dependencies for consistent output
    std::sort(dependencies.begin(), dependencies.end());
    
    // Print dependencies
    std::string nextIndent = indent + (isLast ? "    " : "│   ");
    for (size_t i = 0; i < dependencies.size(); ++i)
    {
        bool isLastDep = (i == dependencies.size() - 1);
        printTreeNode(dependencies[i].first, dependencies[i].second, nextIndent, isLastDep, visited);
    }
    
    visited.erase(nodeKey);
}

std::string SectionUsageAnalyzer::getSymbolDetails(const std::string& symbolName) const
{
    auto it = m_symbolInfo.find(symbolName);
    if (it != m_symbolInfo.end())
    {
        const SymbolInfo& info = *it->second;
        std::string details = " (";
        
        if (info.isFunction)
            details += "func";
        else
            details += "var";
            
        if (info.isGlobal)
            details += ", global";
        else if (info.isWeak)
            details += ", weak";
            
        // Always show section info if available, with object file for disambiguation
        if (!info.sectionName.empty())
        {
            details += ", in " + info.sectionName + " from " + info.unit->getObjectPath().filename().string();
        }
        else
        {
            details += ", from " + info.unit->getObjectPath().filename().string();
        }

        details += ")";
        return details;
    }
    else
    {
        // This might be an external symbol
        return " (external)";
    }
}

std::string SectionUsageAnalyzer::getSectionDetails(const std::string& sectionName) const
{
    for (const auto& sectionInfo : m_sectionUsageInfo)
    {
        if (sectionInfo->name == sectionName)
        {
            std::string details = " (size: " + std::to_string(sectionInfo->size) + " bytes";
            
            details += ", from " + sectionInfo->unit->getObjectPath().filename().string();

            if (sectionInfo->isEntryPoint)
                details += ", entry point";
                
            details += ")";
            return details;
        }
    }
    return " (unknown section)";
}

SectionUsageInfo* SectionUsageAnalyzer::findSection(const std::string& sectionName, const core::CompilationUnit* unit)
{
	// Use fast lookup for specific unit
	SectionKey key(sectionName, unit);
	auto it = m_sectionLookup.find(key);
	if (it != m_sectionLookup.end())
	{
		return it->second;
	}
    return nullptr;
}

const SectionUsageInfo* SectionUsageAnalyzer::findSection(const std::string& sectionName, const core::CompilationUnit* unit) const
{
    return const_cast<SectionUsageAnalyzer*>(this)->findSection(sectionName, unit);
}
