#pragma once

#include <vector>
#include <memory>
#include <string>
#include <unordered_set>
#include <unordered_map>

#include "../utils/types.hpp"
#include "../system/cache.hpp"
#include "../formats/elf.hpp"
#include "../formats/archive.hpp"
#include "../core/compilation_unit_manager.hpp"
#include "types.hpp"

namespace ncp::patch {

class DependencyResolver
{
public:
	struct Symbol
	{
		std::string name;
		std::string sectionName;
		core::CompilationUnit* unit;
		bool isFunction = false;
		bool isGlobal = false;
		bool isWeak = false;
		u32 address = 0;
		u32 size = 0;
	};

	struct UnitEntryPoints {
		core::CompilationUnit* unit;
		std::vector<std::string> sections;
		std::vector<std::string> symbols;
	};

    DependencyResolver();
    ~DependencyResolver();

    void initialize(
		const core::CompilationUnitManager& compilationUnitMgr
	);

    void analyzeObjectFiles();

	void propagateUsage(const std::vector<std::unique_ptr<UnitEntryPoints>>& entryPoints);

	/**
	 * Filters the given sections to only include those that would survive linking.
	 * This removes unused sections based on the dependency analysis.
	 */
	void excludeUnusedSections(std::vector<std::unique_ptr<SectionInfo>>& candidateSections);

    /**
     * Prints a dependency tree from entry points to all used functions/sections.
     * Shows the hierarchical relationships between patches, external symbols, and the code they use.
     */
    void printDependencyTree() const;

    Symbol* findSymbol(const std::string& symbolName);
    const Symbol* findSymbol(const std::string& symbolName) const;
	
    bool isSectionMarkedAsUsed(const std::string& sectionName, const core::CompilationUnit* unit) const;
    bool isSymbolInSection(const std::string& symbolName, const std::string& sectionName) const;

private:
	struct ReferencedSymbol
	{
		std::string name;
		bool isSection;
		
		ReferencedSymbol(const std::string& n, bool section) : name(n), isSection(section) {}
		
		bool operator==(const ReferencedSymbol& other) const {
			return name == other.name && isSection == other.isSection;
		}
	};

	struct ReferencedSymbolHash
	{
		std::size_t operator()(const ReferencedSymbol& rs) const {
			return std::hash<std::string>{}(rs.name) ^ (std::hash<bool>{}(rs.isSection) << 1);
		}
	};

    // Composite key for uniquely identifying sections: sectionName + unit pointer
    struct SectionKey
    {
        std::string sectionName;
        const core::CompilationUnit* unit;
        
        SectionKey(const std::string& section, const core::CompilationUnit* u) 
            : sectionName(section), unit(u) {}
        
        bool operator==(const SectionKey& other) const {
            return sectionName == other.sectionName && unit == other.unit;
        }
    };
    
    struct SectionKeyHash
    {
        std::size_t operator()(const SectionKey& key) const {
            return std::hash<std::string>{}(key.sectionName) ^ 
                   (std::hash<const void*>{}(key.unit) << 1);
        }
    };

	struct Section
	{
		std::string name;
		std::size_t size;
		core::CompilationUnit* unit;
		u32 alignment;
		bool isReferenced = false;
		bool isEntryPoint = false;
		std::unordered_set<ReferencedSymbol, ReferencedSymbolHash> referencedSymbols;
	};

    const std::vector<std::unique_ptr<UnitEntryPoints>>* m_entryPoints;
	const core::CompilationUnitManager* m_compilationUnitMgr;

    std::vector<std::unique_ptr<Section>> m_sectionInfo;
    std::unordered_map<std::string, std::unique_ptr<Symbol>> m_symbolInfo;
    std::unordered_set<std::string> m_referencedSymbols;
    std::unordered_set<Section*> m_markedSections;
    
    // Fast lookup map for sections by name+job
    std::unordered_map<SectionKey, Section*, SectionKeyHash> m_sectionLookup;

    // Analysis methods
    void collectSymbolsAndSectionsWithRelocations();
    void markEntryPoints();
    void propagateUsage();
    
    // Helper methods
    void markSymbolAsUsed(const std::string& symbolName);
    void markSectionAsUsed(const std::string& sectionName, const core::CompilationUnit* unit);
    
    // Section lookup helpers
    Section* findSection(const std::string& sectionName, const core::CompilationUnit* unit);
    const Section* findSection(const std::string& sectionName, const core::CompilationUnit* unit) const;
    
    // Tree printing helpers
    void printTreeNode(const std::string& nodeName, bool isSection, const std::string& indent, bool isLast, std::unordered_set<std::string>& visited) const;
    std::string getSymbolDetails(const std::string& symbolName) const;
    std::string getSectionDetails(const std::string& sectionName) const;
};

} // namespace ncp::patch
