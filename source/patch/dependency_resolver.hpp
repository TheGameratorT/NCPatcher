#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include "../utils/types.hpp"
#include "../app/context.hpp"
#include "../core/compilation_unit_manager.hpp"

namespace ncp::patch {

// Collects the symbol table across every compilation unit, so patches that
// reference an external symbol (for example an ncp_set patch's target) can
// be resolved to an address and section without loading that object's ELF
// again.
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

    DependencyResolver();
    ~DependencyResolver();

    void initialize(
		const ncp::Context& ctx,
		const core::CompilationUnitManager& compilationUnitMgr
	);

    void analyzeObjectFiles();

    Symbol* findSymbol(const std::string& symbolName);
    const Symbol* findSymbol(const std::string& symbolName) const;

private:
	const ncp::Context* m_ctx;
	const core::CompilationUnitManager* m_compilationUnitMgr;

    std::unordered_map<std::string, std::unique_ptr<Symbol>> m_symbolInfo;

    void collectSymbols();
};

} // namespace ncp::patch
