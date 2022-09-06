#include "elf.hpp"

#include <fstream>

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
