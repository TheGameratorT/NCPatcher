#include "dependency_resolver.hpp"

#include "../system/log.hpp"
#include "../formats/elf.hpp"

namespace ncp::patch {

DependencyResolver::DependencyResolver() = default;
DependencyResolver::~DependencyResolver() = default;

void DependencyResolver::initialize(
	const ncp::Context& ctx,
	const core::CompilationUnitManager& compilationUnitMgr
)
{
	m_ctx = &ctx;
	m_compilationUnitMgr = &compilationUnitMgr;
}

void DependencyResolver::analyzeObjectFiles()
{
    m_symbolInfo.clear();

    collectSymbols();

    if (m_ctx->isVerbose(ncp::VerboseTag::Section))
    {
        Log::out << OINFO << "Section usage analysis results:" << std::endl;
        Log::out << "  Total symbols found: " << m_symbolInfo.size() << std::endl;
        Log::out << std::endl;
    }
}

void DependencyResolver::collectSymbols()
{
    for (const auto& unit : m_compilationUnitMgr->getUnits())
    {
        Elf32* elf = unit->getElf();

        const Elf32_Ehdr& eh = elf->getHeader();
        auto sh_tbl = elf->getSectionHeaderTable();
        auto str_tbl = elf->getSection<char>(sh_tbl[eh.e_shstrndx]);

        if (m_ctx->isVerbose(ncp::VerboseTag::Section))
            Log::out << "  Analyzing " << unit->getObjectPath().string() << std::endl;

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

            auto symbolInfo = std::make_unique<Symbol>();
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
                const Symbol& existing = *existingIt->second;

                // If new symbol is strong and existing is weak, replace it
                if (symbolInfo->isGlobal && existing.isWeak)
                {
                    if (m_ctx->isVerbose(ncp::VerboseTag::Symbols))
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
                    if (m_ctx->isVerbose(ncp::VerboseTag::Symbols))
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
                    if (m_ctx->isVerbose(ncp::VerboseTag::Symbols))
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
                    if (m_ctx->isVerbose(ncp::VerboseTag::Symbols))
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
    }
}

DependencyResolver::Symbol* DependencyResolver::findSymbol(const std::string& symbolName)
{
    auto it = m_symbolInfo.find(symbolName);
    if (it != m_symbolInfo.end())
    {
        return it->second.get();
    }
    return nullptr;
}

const DependencyResolver::Symbol* DependencyResolver::findSymbol(const std::string& symbolName) const
{
    return const_cast<DependencyResolver*>(this)->findSymbol(symbolName);
}

} // namespace ncp::patch
