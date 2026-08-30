#include "narc.hpp"

#include <algorithm>
#include <sstream>

#include "layout.hpp"
#include "../utils/endian.hpp"
#include "../system/except.hpp"
#include "../system/log.hpp"

namespace ncp::rom {

namespace {

constexpr std::size_t HEADER_SIZE = 0x10;
constexpr u16 BYTE_ORDER_MARK = 0xFFFE;
constexpr u16 VERSION = 0x0100;
constexpr std::size_t CHUNK_HEADER_SIZE = 8;

// Members are placed on 4-byte boundaries, and the data chunk runs to the
// boundary after the last one. Every archive in the games this was written
// against follows that exactly, which is what makes an untouched archive come
// back out byte-identical.
constexpr u32 ALIGNMENT = 4;

// What the gap between two members is filled with. 0xFF rather than zero: it is
// what the Nitro tooling wrote, and an archive nobody edited has to come back
// out byte-identical down to its padding for that claim to mean anything.
constexpr u8 PADDING = 0xFF;

[[nodiscard]] u32 alignUp(u32 value)
{
	return (value + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1);
}

[[nodiscard]] bool magicAt(std::span<const u8> data, std::size_t offset, const char* magic)
{
	le::requireRange(data, offset, 4);
	return std::equal(magic, magic + 4, data.begin() + std::ptrdiff_t(offset));
}

void writeMagic(std::vector<u8>& out, const char* magic)
{
	out.insert(out.end(), magic, magic + 4);
}

void appendU16(std::vector<u8>& out, u16 value)
{
	out.push_back(u8(value & 0xFF));
	out.push_back(u8((value >> 8) & 0xFF));
}

void appendU32(std::vector<u8>& out, u32 value)
{
	appendU16(out, u16(value & 0xFFFF));
	appendU16(out, u16((value >> 16) & 0xFFFF));
}

} // namespace

bool isNarc(std::span<const u8> data)
{
	return data.size() >= HEADER_SIZE && magicAt(data, 0, "NARC");
}

Narc Narc::parse(std::span<const u8> data)
{
	if (!isNarc(data))
		throw ncp::exception("Not a Nitro archive: the file does not begin with "
			ANSI_bWHITE "\"NARC\"" ANSI_RESET ".");

	if (le::readU16(data, 4) != BYTE_ORDER_MARK)
		throw ncp::exception("Unsupported Nitro archive: it is not little-endian.");

	const u16 version = le::readU16(data, 6);
	if (version != VERSION)
	{
		std::ostringstream oss;
		oss << "Unsupported Nitro archive version 0x" << std::hex << version << ".";
		throw ncp::exception(oss.str());
	}

	const u32 declaredSize = le::readU32(data, 8);
	if (declaredSize != data.size())
	{
		std::ostringstream oss;
		oss << "Malformed Nitro archive: its header claims " << declaredSize
		    << " bytes but the file is " << data.size() << ".";
		throw ncp::exception(oss.str());
	}

	if (le::readU16(data, 12) != HEADER_SIZE)
		throw ncp::exception("Malformed Nitro archive: unexpected header size.");

	// Walk the chunks rather than assuming the documented order. The three that
	// matter are named, and an archive carrying a fourth is not something to
	// guess about.
	std::size_t btaf = 0;
	std::size_t btnf = 0;
	std::size_t btnfSize = 0;
	std::size_t gmif = 0;
	std::size_t gmifSize = 0;

	std::size_t cursor = HEADER_SIZE;
	while (cursor < data.size())
	{
		le::requireRange(data, cursor, CHUNK_HEADER_SIZE);
		const u32 size = le::readU32(data, cursor + 4);
		if (size < CHUNK_HEADER_SIZE || size > data.size() - cursor)
		{
			std::ostringstream oss;
			oss << "Malformed Nitro archive: a chunk at offset " << cursor
			    << " claims " << size << " bytes.";
			throw ncp::exception(oss.str());
		}

		if (magicAt(data, cursor, "BTAF"))
			btaf = cursor;
		else if (magicAt(data, cursor, "BTNF"))
			btnf = cursor, btnfSize = size;
		else if (magicAt(data, cursor, "GMIF"))
			gmif = cursor, gmifSize = size;
		else
		{
			std::ostringstream oss;
			oss << "Unsupported Nitro archive: unknown chunk "
			    << OSTR(std::string(reinterpret_cast<const char*>(data.data()) + cursor, 4)) << ".";
			throw ncp::exception(oss.str());
		}

		cursor += size;
	}

	if (btaf == 0 || btnf == 0 || gmif == 0)
		throw ncp::exception("Malformed Nitro archive: it is missing one of BTAF, BTNF or GMIF.");

	Narc narc;

	const std::size_t count = le::readU16(data, btaf + 8);
	const std::size_t dataStart = gmif + CHUNK_HEADER_SIZE;
	const std::size_t dataSize = gmifSize - CHUNK_HEADER_SIZE;

	narc.m_files.resize(count);
	for (std::size_t i = 0; i < count; i++)
	{
		const u32 start = le::readU32(data, btaf + 12 + i * 8);
		const u32 end = le::readU32(data, btaf + 12 + i * 8 + 4);
		if (end < start || end > dataSize)
		{
			std::ostringstream oss;
			oss << "Malformed Nitro archive: member " << i << " spans " << start << " to " << end
			    << ", which is outside its " << dataSize << "-byte data chunk.";
			throw ncp::exception(oss.str());
		}
		narc.m_files[i].assign(
			data.begin() + std::ptrdiff_t(dataStart + start),
			data.begin() + std::ptrdiff_t(dataStart + end));
	}

	narc.m_names.assign(
		data.begin() + std::ptrdiff_t(btnf + CHUNK_HEADER_SIZE),
		data.begin() + std::ptrdiff_t(btnf + btnfSize));
	narc.m_tree = NitroFs::parse(narc.m_names);

	return narc;
}

std::vector<u8> Narc::serialize() const
{
	if (m_files.size() > 0xFFFF)
		throw ncp::exception("A Nitro archive cannot hold more than 65535 files.");

	std::vector<u8> allocation;
	std::vector<u8> contents;
	allocation.reserve(4 + m_files.size() * 8);

	appendU16(allocation, u16(m_files.size()));
	appendU16(allocation, 0);
	for (const std::vector<u8>& file : m_files)
	{
		const u32 start = u32(contents.size());
		contents.insert(contents.end(), file.begin(), file.end());
		appendU32(allocation, start);
		appendU32(allocation, u32(contents.size()));
		contents.resize(alignUp(u32(contents.size())), PADDING);
	}

	const u32 btafSize = u32(CHUNK_HEADER_SIZE + allocation.size());
	const u32 btnfSize = u32(CHUNK_HEADER_SIZE + m_names.size());
	const u32 gmifSize = u32(CHUNK_HEADER_SIZE + contents.size());

	std::vector<u8> out;
	out.reserve(HEADER_SIZE + btafSize + btnfSize + gmifSize);

	writeMagic(out, "NARC");
	appendU16(out, BYTE_ORDER_MARK);
	appendU16(out, VERSION);
	appendU32(out, u32(HEADER_SIZE) + btafSize + btnfSize + gmifSize);
	appendU16(out, u16(HEADER_SIZE));
	appendU16(out, 3);

	writeMagic(out, "BTAF");
	appendU32(out, btafSize);
	out.insert(out.end(), allocation.begin(), allocation.end());

	writeMagic(out, "BTNF");
	appendU32(out, btnfSize);
	out.insert(out.end(), m_names.begin(), m_names.end());

	writeMagic(out, "GMIF");
	appendU32(out, gmifSize);
	out.insert(out.end(), contents.begin(), contents.end());

	return out;
}

int Narc::findFile(std::string_view path) const
{
	const int id = m_tree.findFile(path);
	if (id < 0 || std::size_t(id) >= m_files.size())
		return -1;
	return id;
}

std::span<const u8> Narc::file(std::size_t index) const
{
	return m_files.at(index);
}

void Narc::replaceFile(std::size_t index, std::vector<u8> data)
{
	m_files.at(index) = std::move(data);
}

std::vector<std::pair<u32, std::string>> Narc::allFiles() const
{
	return m_tree.allFiles();
}

} // namespace ncp::rom
