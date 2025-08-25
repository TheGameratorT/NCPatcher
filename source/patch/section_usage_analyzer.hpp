#pragma once

#include <vector>
#include <memory>
#include <string>
#include <unordered_set>
#include <unordered_map>

#include "../utils/types.hpp"
#include "../formats/elf.hpp"
#include "../core/compilation_unit_manager.hpp"
#include "types.hpp"

// Use the centralized types from patch::types
using patch::SectionInfo;
using patch::SectionUsageInfo;
using patch::SymbolInfo;
using patch::GenericPatchInfo;
using patch::ReferencedSymbol;
using patch::ReferencedSymbolHash;

/**
 * Analyzes object files to determine section usage without requiring ELF linking.
 * This replaces the complex 2-step ELF generation system with direct object analysis.
 */
class SectionUsageAnalyzer
{
public:
    SectionUsageAnalyzer();
    ~SectionUsageAnalyzer();

    void initialize(
        const std::vector<std::unique_ptr<GenericPatchInfo>>& patchInfo,
        const std::vector<std::string>& externSymbols,
    	core::CompilationUnitManager& compilationUnitMgr
    );

    /**
     * Analyzes all object files to determine which sections are actually used.
     * This considers:
     * - Patch entry points (functions/sections referenced by patches)
     * - External symbols that must be kept
     * - Inter-object symbol dependencies
     * - Relocations between sections
     */
    void analyzeObjectFiles();

    /**
     * Filters candidate sections to only include those that would survive linking.
     * This is much more accurate than the current strip ELF approach because it
     * analyzes the actual object-level dependencies.
     */
    void filterUsedSections(std::vector<std::unique_ptr<SectionInfo>>& candidateSections);

    /**
     * Prints a dependency tree from entry points to all used functions/sections.
     * Shows the hierarchical relationships between patches, external symbols, and the code they use.
     */
    void printDependencyTree() const;

    // Getters for debugging/verbose output
    const std::vector<std::unique_ptr<SectionUsageInfo>>& getSectionUsageInfo() const { return m_sectionUsageInfo; }
    const std::unordered_map<std::string, std::unique_ptr<SymbolInfo>>& getSymbolInfo() const { return m_symbolInfo; }

private:
    const std::vector<std::unique_ptr<GenericPatchInfo>>* m_patchInfo;
    const std::vector<std::string>* m_externSymbols;
    core::CompilationUnitManager* m_compilationUnitMgr;

    // Composite key for uniquely identifying sections: sectionName + objectFileName
    struct SectionKey
    {
        std::string sectionName;
        std::string objectFileName;
        
        SectionKey(const std::string& section, const std::string& object) 
            : sectionName(section), objectFileName(object) {}
        
        bool operator==(const SectionKey& other) const {
            return sectionName == other.sectionName && objectFileName == other.objectFileName;
        }
    };
    
    struct SectionKeyHash
    {
        std::size_t operator()(const SectionKey& key) const {
            return std::hash<std::string>{}(key.sectionName) ^ 
                   (std::hash<std::string>{}(key.objectFileName) << 1);
        }
    };

    std::vector<std::unique_ptr<SectionUsageInfo>> m_sectionUsageInfo;
    std::unordered_map<std::string, std::unique_ptr<SymbolInfo>> m_symbolInfo;
    std::unordered_set<std::string> m_referencedSymbols;
    std::unordered_set<SectionUsageInfo*> m_markedSections; // Renamed for clarity
    
    // Fast lookup map for sections by name+job
    std::unordered_map<SectionKey, SectionUsageInfo*, SectionKeyHash> m_sectionLookup;

    // Analysis methods
    void collectSymbolsAndSections();
    void analyzeRelocations();
    void markEntryPoints();
    void propagateUsage();
    
    // Helper methods
    void markSymbolAsUsed(const std::string& symbolName);
    void markSectionAsUsed(const std::string& sectionName, const core::CompilationUnit* unit);
    bool isSectionMarkedAsUsed(const std::string& sectionName, const core::CompilationUnit* unit) const;
    bool isSymbolInSection(const std::string& symbolName, const std::string& sectionName);
    bool loadElfFromArchive(Elf32& elf, const std::filesystem::path& archivePath, const std::string& memberName);
    
    // Section lookup helpers
    SectionUsageInfo* findSection(const std::string& sectionName, const core::CompilationUnit* unit);
    const SectionUsageInfo* findSection(const std::string& sectionName, const core::CompilationUnit* unit) const;
    void buildSectionLookupMap();
    
    // Tree printing helpers
    void printTreeNode(const std::string& nodeName, bool isSection, const std::string& indent, bool isLast, std::unordered_set<std::string>& visited) const;
    std::string getSymbolDetails(const std::string& symbolName) const;
    std::string getSectionDetails(const std::string& sectionName) const;
};
