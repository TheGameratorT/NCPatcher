#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string_view>

typedef uint32_t Elf32_Addr;
typedef uint16_t Elf32_Half;
typedef uint32_t Elf32_Off;
typedef  int32_t Elf32_Sword;
typedef uint32_t Elf32_Word;

#define ELF_ST_BIND(x)   ((x) >> 4)
#define ELF_ST_TYPE(x)   (((unsigned int) x) & 0xf)
#define ELF32_ST_BIND(x) ELF_ST_BIND(x)
#define ELF32_ST_TYPE(x) ELF_ST_TYPE(x)

#define ELF32_R_SYM(i)	  ((i)>>8)
#define ELF32_R_TYPE(i)   ((unsigned char)(i))
#define ELF32_R_INFO(s,t) (((s)<<8)+(unsigned char)(t))

#define SHT_NULL     0
#define SHT_PROGBITS 1
#define SHT_SYMTAB   2
#define SHT_STRTAB   3
#define SHT_RELA     4
#define SHT_HASH     5
#define SHT_DYNAMIC  6
#define SHT_NOTE     7
#define SHT_NOBITS   8
#define SHT_REL      9
#define SHT_SHLIB    10
#define SHT_DYNSYM   11
#define SHT_NUM      12
#define SHT_LOPROC   0x70000000
#define SHT_HIPROC   0x7fffffff
#define SHT_LOUSER   0x80000000
#define SHT_HIUSER   0xffffffff

#define SHN_UNDEF     0x0
#define SHF_WRITE     0x1
#define SHF_ALLOC     0x2
#define SHF_EXECINSTR 0x4
#define SHF_MASKPROC  0xf0000000

#define STB_LOCAL  0
#define STB_GLOBAL 1
#define STB_WEAK   2

#define STT_NOTYPE  0
#define STT_OBJECT  1
#define STT_FUNC    2
#define STT_SECTION 3
#define STT_FILE    4
#define STT_COMMON  5
#define STT_TLS     6

#define PF_R 0x4
#define PF_W 0x2
#define PF_X 0x1

struct Elf32_Rel
{
	Elf32_Addr r_offset; // 0x00 | Location at which to apply the action
	Elf32_Word r_info;   // 0x04 | Index and type of relocation
};

struct Elf32_Rela
{
	Elf32_Addr r_offset;  // 0x00 | Location at which to apply the action
	Elf32_Word r_info;    // 0x04 | Index and type of relocation
	Elf32_Sword r_addend; // 0x08 | Constant addend used to compute value
};

struct Elf32_Sym
{
	Elf32_Word st_name;     // 0x00 | Symbol name, index in string tbl
	Elf32_Addr st_value;    // 0x04 | Value of the symbol
	Elf32_Word st_size;     // 0x08 | Associated symbol size
	unsigned char st_info;  // 0x0C | Type and binding attributes
	unsigned char st_other; // 0x0D | No defined meaning, 0
	Elf32_Half st_shndx;    // 0x0E | Associated section index
};

struct Elf32_Ehdr
{
	unsigned char e_ident[16]; // 0x00 | ELF "magic number"
	Elf32_Half e_type;         // 0x10 | 
	Elf32_Half e_machine;      // 0x12 | 
	Elf32_Word e_version;      // 0x14 | 
	Elf32_Addr e_entry;        // 0x18 | Entry point virtual address
	Elf32_Off e_phoff;         // 0x1C | Program header table file offset
	Elf32_Off e_shoff;         // 0x20 | Section header table file offset
	Elf32_Word e_flags;        // 0x24 | 
	Elf32_Half e_ehsize;       // 0x28 | ELF header size
	Elf32_Half e_phentsize;    // 0x2A | Program header entry size
	Elf32_Half e_phnum;        // 0x2C | Program header count
	Elf32_Half e_shentsize;    // 0x2E | Section header entry size
	Elf32_Half e_shnum;        // 0x30 | Section header count
	Elf32_Half e_shstrndx;     // 0x32 | Section header string table index
};

struct Elf32_Phdr
{
	Elf32_Word p_type;   // 0x00 | Segment type
	Elf32_Off p_offset;  // 0x04 | Segment file offset
	Elf32_Addr p_vaddr;  // 0x08 | Segment virtual address
	Elf32_Addr p_paddr;  // 0x0C | Segment physical address
	Elf32_Word p_filesz; // 0x10 | Segment size in file
	Elf32_Word p_memsz;  // 0x14 | Segment size in memory
	Elf32_Word p_flags;  // 0x18 | Segment flags
	Elf32_Word p_align;  // 0x1C | Segment alignment, file & memory
};

struct Elf32_Shdr
{
	Elf32_Word sh_name;      // 0x00 | Section name, index in string tbl
	Elf32_Word sh_type;      // 0x04 | Type of section
	Elf32_Word sh_flags;     // 0x08 | Miscellaneous section attributes
	Elf32_Addr sh_addr;      // 0x0C | Section virtual addr at execution
	Elf32_Off sh_offset;     // 0x10 | Section file offset
	Elf32_Word sh_size;      // 0x14 | Size of section in bytes
	Elf32_Word sh_link;      // 0x18 | Index of another section
	Elf32_Word sh_info;      // 0x1C | Additional section information
	Elf32_Word sh_addralign; // 0x20 | Section alignment
	Elf32_Word sh_entsize;   // 0x24 | Entry size if section holds table
};

class Elf32
{
public:
	Elf32();
	~Elf32();

	bool load(const std::filesystem::path& elf);
	bool loadFromMemory(const char* data, std::size_t size);

	[[nodiscard]] inline const Elf32_Ehdr& getHeader() const {
		return *reinterpret_cast<const Elf32_Ehdr*>(dataptr);
	};

	[[nodiscard]] inline const Elf32_Phdr* getProgramHeaderTable() const {
		return reinterpret_cast<const Elf32_Phdr*>(dataptr + getHeader().e_phoff);
	}

	[[nodiscard]] inline const Elf32_Shdr* getSectionHeaderTable() const {
		return reinterpret_cast<const Elf32_Shdr*>(dataptr + getHeader().e_shoff);
	}

	template<typename T>
	constexpr const T* getSection(const Elf32_Shdr& sh) const {
		return reinterpret_cast<const T*>(dataptr + sh.sh_offset);
	}

	// ELF iteration utilities
	static void forEachSection(
		const Elf32_Ehdr& eh, const Elf32_Shdr* sh_tbl, const char* str_tbl,
		const std::function<bool(std::size_t, const Elf32_Shdr&, std::string_view)>& cb
	);

	static void forEachSymbol(
		const Elf32& elf, const Elf32_Ehdr& eh, const Elf32_Shdr* sh_tbl,
		const std::function<bool(const Elf32_Sym&, std::string_view)>& cb
	);

	static void forEachRelocation(
		const Elf32& elf, const Elf32_Ehdr& eh, const Elf32_Shdr* sh_tbl,
		const std::function<bool(const Elf32_Rel&, std::string_view, std::string_view)>& cb
	);

private:
	char* dataptr;
};
