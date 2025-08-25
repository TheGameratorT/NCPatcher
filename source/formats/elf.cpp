#include "elf.hpp"

#include <fstream>
#include <cstring>

Elf32::Elf32() :
	dataptr(nullptr)
{}

Elf32::~Elf32()
{
	delete[] dataptr;
}

bool Elf32::load(const std::filesystem::path& elf)
{
	std::uintmax_t fs = std::filesystem::file_size(elf);
	std::ifstream ef(elf, std::ios::binary);
	if (!ef.is_open())
		return false;
	dataptr = new char[fs];
	ef.read(dataptr, std::streamsize(fs));
	ef.close();
	return true;
}

bool Elf32::loadFromMemory(const char* data, std::size_t size)
{
	if (!data || size == 0)
		return false;
		
	// Clean up any existing data
	delete[] dataptr;
	
	// Allocate new memory and copy the data
	dataptr = new char[size];
	std::memcpy(dataptr, data, size);
	
	return true;
}

void Elf32::forEachSection(
	const Elf32_Ehdr& eh, const Elf32_Shdr* sh_tbl, const char* str_tbl,
	const std::function<bool(std::size_t, const Elf32_Shdr&, std::string_view)>& cb
)
{
	for (std::size_t i = 0; i < eh.e_shnum; i++)
	{
		const Elf32_Shdr& sh = sh_tbl[i];
		std::string_view sectionName(&str_tbl[sh.sh_name]);
		if (cb(i, sh, sectionName))
			break;
	}
}

void Elf32::forEachSymbol(
	const Elf32& elf, const Elf32_Ehdr& eh, const Elf32_Shdr* sh_tbl,
	const std::function<bool(const Elf32_Sym&, std::string_view)>& cb
)
{
	for (std::size_t i = 0; i < eh.e_shnum; i++)
	{
		const Elf32_Shdr& sh = sh_tbl[i];
		if ((sh.sh_type == SHT_SYMTAB) || (sh.sh_type == SHT_DYNSYM))
		{
			auto sym_tbl = elf.getSection<Elf32_Sym>(sh);
			auto sym_str_tbl = elf.getSection<char>(sh_tbl[sh.sh_link]);
			for (std::size_t j = 0; j < sh.sh_size / sizeof(Elf32_Sym); j++)
			{
				const Elf32_Sym& sym = sym_tbl[j];
				std::string_view symbolName(&sym_str_tbl[sym.st_name]);
				if (cb(sym, symbolName))
					break;
			}
		}
	}
}

void Elf32::forEachRelocation(
	const Elf32& elf, const Elf32_Ehdr& eh, const Elf32_Shdr* sh_tbl,
	const std::function<bool(const Elf32_Rel&, std::string_view, std::string_view)>& cb
)
{
	const char* str_tbl = elf.getSection<char>(sh_tbl[eh.e_shstrndx]);
	
	for (std::size_t i = 0; i < eh.e_shnum; i++)
	{
		const Elf32_Shdr& sh = sh_tbl[i];
		if (sh.sh_type != SHT_REL)
			continue;
			
		std::string_view sectionName(&str_tbl[sh.sh_name]);
		
		// Get the section that these relocations apply to
		std::string_view targetSectionName = "";
		if (sectionName.starts_with(".rel"))
		{
			targetSectionName = sectionName.substr(4); // Remove ".rel" prefix
		}
		
		auto rel_tbl = elf.getSection<Elf32_Rel>(sh);
		std::size_t relCount = sh.sh_size / sizeof(Elf32_Rel);
		
		for (std::size_t j = 0; j < relCount; j++)
		{
			if (cb(rel_tbl[j], sectionName, targetSectionName))
				break;
		}
	}
}
