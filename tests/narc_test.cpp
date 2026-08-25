// Tests for source/rom/narc.cpp -- Nitro archives, the filesystem inside a file.
//
// Two properties carry the weight. An archive nobody edited has to come back out
// byte-identical, because that is the cheapest evidence the writer agrees with
// every archive already in the wild; and a replacement of a different length has
// to relay the allocation table out, since the members after it move. Everything
// else here is refusing malformed input rather than reading past the end of it.

#include "../source/rom/narc.hpp"

#include <functional>
#include <iostream>
#include <string>
#include <vector>

using namespace ncp;

static int g_failures = 0;

static void check(bool condition, const std::string& what)
{
	if (!condition)
	{
		std::cout << "FAIL: " << what << "\n";
		g_failures++;
	}
}

static bool threw(const std::function<void()>& body, std::string_view expected)
{
	try
	{
		body();
	}
	catch (const std::exception& e)
	{
		return std::string(e.what()).find(expected) != std::string::npos;
	}
	return false;
}

static void appendU16(std::vector<u8>& out, u16 value)
{
	out.push_back(u8(value & 0xFF));
	out.push_back(u8(value >> 8));
}

static void appendU32(std::vector<u8>& out, u32 value)
{
	appendU16(out, u16(value & 0xFFFF));
	appendU16(out, u16(value >> 16));
}

static void appendMagic(std::vector<u8>& out, const char* magic)
{
	out.insert(out.end(), magic, magic + 4);
}

// A name table naming `a.bin`, `sub/b.bin` and `sub/c.bin`, in that file order.
// Written out by hand rather than through NitroFs::serialize so that the test
// does not pass by agreeing with the code it is checking.
static std::vector<u8> nameTable()
{
	std::vector<u8> out;

	// Two directory rows: the root, then `sub`.
	appendU32(out, 0x10); // root subtable
	appendU16(out, 0);    // first file id
	appendU16(out, 2);    // the root row holds the directory count
	appendU32(out, 0x1D); // sub's subtable, past the root's six + six + one bytes
	appendU16(out, 1);
	appendU16(out, 0xF000);

	// Root: one file, then the directory.
	out.push_back(0x05);
	for (char c : std::string("a.bin")) out.push_back(u8(c));
	out.push_back(0x80 | 0x03);
	for (char c : std::string("sub")) out.push_back(u8(c));
	appendU16(out, 0xF001);
	out.push_back(0x00);

	// sub: two files.
	out.push_back(0x05);
	for (char c : std::string("b.bin")) out.push_back(u8(c));
	out.push_back(0x05);
	for (char c : std::string("c.bin")) out.push_back(u8(c));
	out.push_back(0x00);

	return out;
}

// Assembles an archive the way every one in a retail cartridge is laid out:
// members on 4-byte boundaries with 0xFF between them, the data chunk running
// to the boundary after the last of them.
static std::vector<u8> buildNarc(const std::vector<std::vector<u8>>& files,
                                 const std::vector<u8>& names)
{
	std::vector<u8> allocation;
	std::vector<u8> contents;
	appendU16(allocation, u16(files.size()));
	appendU16(allocation, 0);
	for (const std::vector<u8>& file : files)
	{
		const u32 start = u32(contents.size());
		contents.insert(contents.end(), file.begin(), file.end());
		appendU32(allocation, start);
		appendU32(allocation, u32(contents.size()));
		contents.resize((contents.size() + 3) & ~std::size_t(3), 0xFF);
	}

	const u32 btaf = u32(8 + allocation.size());
	const u32 btnf = u32(8 + names.size());
	const u32 gmif = u32(8 + contents.size());

	std::vector<u8> out;
	appendMagic(out, "NARC");
	appendU16(out, 0xFFFE);
	appendU16(out, 0x0100);
	appendU32(out, 0x10 + btaf + btnf + gmif);
	appendU16(out, 0x10);
	appendU16(out, 3);

	appendMagic(out, "BTAF");
	appendU32(out, btaf);
	out.insert(out.end(), allocation.begin(), allocation.end());

	appendMagic(out, "BTNF");
	appendU32(out, btnf);
	out.insert(out.end(), names.begin(), names.end());

	appendMagic(out, "GMIF");
	appendU32(out, gmif);
	out.insert(out.end(), contents.begin(), contents.end());
	return out;
}

static std::vector<u8> filled(std::size_t size, u8 value)
{
	return std::vector<u8>(size, value);
}

int main()
{
	const std::vector<std::vector<u8>> members = {
		filled(9, 0xA1),   // deliberately not a multiple of four
		filled(4, 0xB2),
		filled(17, 0xC3),
	};
	const std::vector<u8> original = buildNarc(members, nameTable());

	check(rom::isNarc(original), "the magic is recognised");
	check(!rom::isNarc(filled(64, 0)), "a file of zeroes is not an archive");

	{
		rom::Narc narc = rom::Narc::parse(original);
		check(narc.fileCount() == 3, "every member is read");
		check(narc.serialize() == original, "an untouched archive round-trips byte-identically");
	}

	{
		rom::Narc narc = rom::Narc::parse(original);
		check(narc.findFile("a.bin") == 0, "a root member resolves to its index");
		check(narc.findFile("sub/b.bin") == 1, "a nested member resolves");
		check(narc.findFile("sub/c.bin") == 2, "file ids continue through the subtable");
		check(narc.findFile("sub/missing.bin") == -1, "an absent member resolves to -1");
		check(narc.findFile("sub") == -1, "a directory is not a member");

		const std::vector<std::pair<u32, std::string>> listed = narc.allFiles();
		check(listed.size() == 3, "the listing names every member");
		check(!listed.empty() && listed[1].second == "sub/b.bin", "the listing carries full paths");
	}

	{
		// Same size in, same bytes out everywhere but the member itself: the
		// allocation table cannot need touching.
		rom::Narc narc = rom::Narc::parse(original);
		narc.replaceFile(0, filled(9, 0x5A));
		const std::vector<u8> written = narc.serialize();
		check(written.size() == original.size(), "a same-size replacement keeps the length");

		rom::Narc reread = rom::Narc::parse(written);
		check(reread.file(0).size() == 9 && reread.file(0)[0] == 0x5A, "the new bytes are there");
		check(reread.file(2).size() == 17 && reread.file(2)[0] == 0xC3, "its neighbours are not");
	}

	{
		// The case the allocation table exists for. Member 0 grows past its
		// slot, so 1 and 2 move and every offset after it has to be rewritten.
		rom::Narc narc = rom::Narc::parse(original);
		narc.replaceFile(0, filled(100, 0x77));
		const std::vector<u8> written = narc.serialize();

		rom::Narc reread = rom::Narc::parse(written);
		check(reread.fileCount() == 3, "the member count is unchanged");
		check(reread.file(0).size() == 100, "the grown member kept its bytes");
		check(reread.file(1).size() == 4 && reread.file(1)[0] == 0xB2, "the member after it moved intact");
		check(reread.file(2).size() == 17 && reread.file(2)[0] == 0xC3, "and so did the one after that");
		check(reread.findFile("sub/c.bin") == 2, "indices did not shift, so the names still point at them");
		check(written == buildNarc({ filled(100, 0x77), members[1], members[2] }, nameTable()),
			"the relaid archive is what a writer starting from scratch would produce");
	}

	{
		// Shrinking is the same rewrite in the other direction, and the data
		// chunk has to shrink with it rather than keeping the slack.
		rom::Narc narc = rom::Narc::parse(original);
		narc.replaceFile(2, filled(1, 0x11));
		const std::vector<u8> written = narc.serialize();
		check(written.size() < original.size(), "the archive got shorter");
		check(rom::Narc::parse(written).file(2).size() == 1, "the shrunk member reads back");
	}

	{
		// Every rejection below is a file someone hands the tool, not a
		// programming error, so each has to produce a message.
		check(threw([&] { (void)rom::Narc::parse(filled(64, 0)); }, "Not a Nitro archive"),
			"a file without the magic is refused");

		std::vector<u8> truncated = original;
		truncated.resize(truncated.size() - 4);
		check(threw([&] { (void)rom::Narc::parse(truncated); }, "header claims"),
			"a length the header disagrees with is refused");

		std::vector<u8> wrongOrder = original;
		wrongOrder[4] = 0xFF;
		wrongOrder[5] = 0xFE;
		check(threw([&] { (void)rom::Narc::parse(wrongOrder); }, "little-endian"),
			"a big-endian archive is refused rather than misread");

		std::vector<u8> unknownChunk = original;
		unknownChunk[0x10] = 'X';
		check(threw([&] { (void)rom::Narc::parse(unknownChunk); }, "unknown chunk"),
			"an unrecognised chunk is refused rather than skipped");
	}

	{
		// A nameless archive is legal -- its members have numbers and nothing
		// else -- and has to survive a round trip like any other.
		std::vector<u8> emptyNames;
		appendU32(emptyNames, 0x08);
		appendU16(emptyNames, 0);
		appendU16(emptyNames, 1);
		emptyNames.push_back(0x00);
		emptyNames.resize(12, 0);

		const std::vector<u8> bytes = buildNarc({ filled(6, 0x0E) }, emptyNames);
		rom::Narc narc = rom::Narc::parse(bytes);
		check(narc.fileCount() == 1, "a nameless archive still reports its members");
		check(narc.findFile("anything") == -1, "nothing resolves by name in it");
		check(narc.serialize() == bytes, "and it round-trips");
	}

	if (g_failures == 0)
		std::cout << "All NARC tests passed\n";
	return g_failures == 0 ? 0 : 1;
}
