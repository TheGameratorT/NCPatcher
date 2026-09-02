// Tests for source/formats/blz.{hpp,cpp}.
//
// Covers the round trip, the cases where compression gives up, and the
// malformed streams the decompressor has to reject rather than run off the end
// of the buffer for.
// Run via ctest, or directly: ./blz_test

#include "../source/formats/blz.hpp"

#include <iostream>
#include <random>
#include <string>
#include <vector>

static int g_failures = 0;

static void check(bool condition, const std::string& what)
{
	if (!condition)
	{
		std::cout << "FAIL: " << what << "\n";
		g_failures++;
	}
}

template <typename F>
static void expectThrow(F&& fn, const std::string& what)
{
	try
	{
		fn();
	}
	catch (const std::exception&)
	{
		return;
	}
	check(false, what + " should have thrown");
}

static void writeU32(std::vector<u8>& data, size_t offset, u32 value)
{
	for (int i = 0; i < 4; i++)
		data[offset + size_t(i)] = u8((value >> (8 * i)) & 0xFF);
}

// Builds an image around a hand-written token stream. The stream is given in
// the order the decompressor reads it (first element first) which is the
// reverse of the order it sits in memory, since the decompressor runs
// downwards from the end.
static std::vector<u8> makeImage(const std::vector<u8>& stream, u32 offsetOut, u32 topOverride = 0)
{
	size_t total = stream.size() + 8;
	const size_t padding = (4 - (total % 4)) % 4;
	total += padding;

	std::vector<u8> out(total, 0);
	for (size_t i = 0; i < stream.size(); i++)
		out[stream.size() - 1 - i] = stream[i];

	const u32 offsetInTop = topOverride != 0 ? topOverride : u32(total);
	writeU32(out, total - 8, offsetInTop | (u32(8 + padding) << 24));
	writeU32(out, total - 4, offsetOut);
	return out;
}

static void testRoundTrip()
{
	// Repetitive enough to be worth encoding, but with enough variation to
	// exercise literals and references in the same flag byte.
	std::vector<u8> data;
	for (int i = 0; i < 4000; i++)
		data.push_back(u8("the quick brown fox "[i % 20] + (i % 97 == 0 ? i & 3 : 0)));

	const std::vector<u8> packed = BLZ::compress(data);
	check(!packed.empty(), "repetitive data compresses");
	check(packed.size() < data.size(), "compressed image is smaller than the input");
	check(packed.size() % 4 == 0, "compressed image is word aligned");

	if (packed.empty())
		return;

	check(BLZ::uncompress(packed) == data, "uncompress(compress(data)) == data");

	std::vector<u8> inPlace = packed;
	BLZ::uncompressInplace(inPlace);
	check(inPlace == data, "uncompressInplace(compress(data)) == data");
}

static void testRoundTripSizes()
{
	// The encoder walks the input in groups of eight tokens, so the sizes that
	// tend to break it are the ones where the last group is partial.
	std::mt19937 rng(20260830);
	for (size_t size = 16; size <= 600; size++)
	{
		std::vector<u8> data(size);
		for (size_t i = 0; i < size; i++)
			data[i] = u8(rng() % 4);

		const std::vector<u8> packed = BLZ::compress(data);
		if (packed.empty())
			continue;

		if (BLZ::uncompress(packed) != data)
		{
			check(false, "round trip at size " + std::to_string(size));
			return;
		}
	}
}

// The slack between the stream and the footer is 0xFF, which is what every
// other BLZ tool writes. Nothing reads it, so the only thing this buys is that
// recompressing an untouched overlay gives back the bytes the ROM shipped --
// which is what makes an extract-and-pack round trip comparable.
static void testFooterPadding()
{
	// Sizes vary the encoded length, and one of the four alignments has to be
	// the one that leaves slack.
	bool sawPadding = false;
	for (size_t size = 200; size < 260; size++)
	{
		std::vector<u8> data(size, 0);
		for (size_t i = 0; i < size; i++)
			data[i] = u8("abcabcabd"[i % 9]);

		const std::vector<u8> packed = BLZ::compress(data);
		if (packed.empty())
			continue;

		// The footer says how much of the tail is not stream: eight bytes of
		// footer plus the slack.
		const u32 offsetIn = u32(packed[packed.size() - 8]) | (u32(packed[packed.size() - 7]) << 8)
			| (u32(packed[packed.size() - 6]) << 16) | (u32(packed[packed.size() - 5]) << 24);
		const size_t padding = (offsetIn >> 24) - 8;
		if (padding == 0)
			continue;

		sawPadding = true;
		for (size_t i = 0; i < padding; i++)
		{
			if (packed[packed.size() - 8 - padding + i] != 0xFF)
			{
				check(false, "footer slack is 0xFF at size " + std::to_string(size));
				return;
			}
		}

		if (BLZ::uncompress(packed) != data)
		{
			check(false, "a padded image still round trips at size " + std::to_string(size));
			return;
		}
	}

	check(sawPadding, "some size leaves slack before the footer");
}

static void testIncompressible()
{
	std::mt19937 rng(1);
	std::vector<u8> noise(4000);
	for (u8& b : noise)
		b = u8(rng());

	check(BLZ::compress(noise).empty(), "random data does not compress");

	std::vector<u8> tiny(15, 0);
	check(BLZ::compress(tiny).empty(), "data below the minimum size does not compress");
}

static void testLiterals()
{
	const std::vector<u8> stream = { 0x00, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h' };
	const std::vector<u8> out = BLZ::uncompress(makeImage(stream, 8));

	check(out.size() == makeImage(stream, 8).size() + 8, "literal image grows by offsetOut");
	check(std::string(out.end() - 8, out.end()) == "hgfedcba", "literals land in reverse order");
}

static void testReference()
{
	// Eight literals, then a reference three back for three bytes, which is the
	// shortest thing the format can encode.
	const std::vector<u8> stream = {
		0x00, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h',
		0x80, 0x00, 0x00
	};
	const std::vector<u8> out = BLZ::uncompress(makeImage(stream, 11));

	check(std::string(out.end() - 11, out.end()) == "hgfhgfedcba", "reference copies backwards");
}

// A reference may be longer than its distance, so the copy reads bytes it is
// itself writing. BLZ::compress never emits one (findMatch caps the length at
// the distance), so nothing that round trips through it covers this path, but
// Nintendo's compressor emits them constantly and every retail ARM9 is full of
// them. It is also what the decode loop MSVC 19.51 miscompiles spends its time
// doing, so this is the test that catches that.
static void testOverlappingReferences()
{
	constexpr size_t REFS = 2000;  // a multiple of 8, so no partial flag group
	constexpr size_t LITERALS = 8;
	constexpr size_t REF_LENGTH = 18;

	std::vector<u8> stream = { 0x00, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h' };
	for (size_t i = 0; i < REFS; i++)
	{
		if (i % 8 == 0)
			stream.push_back(0xFF);
		stream.push_back(0xF0);  // length 18
		stream.push_back(0x00);  // distance 3
	}

	const size_t outSize = LITERALS + REFS * REF_LENGTH;
	const size_t imageSize = ((stream.size() + 8) + 3) & ~size_t(3);

	// Each reference byte is a copy of the one three above it, so the output is
	// the eight literals with a period-three pattern extending down from them.
	std::vector<u8> expected(outSize);
	const char* const literals = "hgfedcba";
	for (size_t i = 0; i < LITERALS; i++)
		expected[outSize - LITERALS + i] = u8(literals[i]);
	for (size_t i = outSize - LITERALS; i-- > 0; )
		expected[i] = expected[i + 3];

	check(BLZ::uncompress(makeImage(stream, u32(outSize - imageSize))) == expected,
		"references longer than their distance repeat a pattern");
}

static void testMalformed()
{
	// A reference before anything has been decoded reaches past the end of the
	// output buffer.
	expectThrow([] { BLZ::uncompress(makeImage({ 0x80, 0x00, 0x00 }, 8)); },
		"a reference with nothing decoded yet");

	// A flag byte with no tokens behind it.
	expectThrow([] { BLZ::uncompress(makeImage({ 0x00 }, 8)); },
		"a flag byte with no token");

	// A reference token cut short by the start of the stream.
	expectThrow([] { BLZ::uncompress(makeImage({ 0x80, 0x00 }, 8)); },
		"a truncated reference token");

	// The stream's far end is nearer than its near end.
	expectThrow([] { BLZ::uncompress(makeImage({ 0x00, 'a' }, 8, 2)); },
		"a header whose ends are the wrong way round");

	// The longest reference the format can express, with less than that much
	// room left below the write cursor.
	expectThrow([] {
		BLZ::uncompress(makeImage({ 0x00, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 0x80, 0xF0, 0x00 }, 0));
	}, "a reference running past the start of the output");

	// The stream starts before the image does.
	expectThrow([] { BLZ::uncompress(makeImage({ 0x00, 'a' }, 8, 999)); },
		"a stream reaching above the image");

	// A near end that is neither the footer nor the footer plus padding.
	expectThrow([] {
		std::vector<u8> image = makeImage({ 0x00, 'a' }, 8);
		image[image.size() - 5] = 0x40;
		BLZ::uncompress(image);
	}, "a near end that is not the footer");

	// Too small to hold a footer at all.
	expectThrow([] { BLZ::uncompress(std::vector<u8>(4, 0)); }, "an image shorter than its footer");
}

int main()
{
	testRoundTrip();
	testRoundTripSizes();
	testFooterPadding();
	testIncompressible();
	testLiterals();
	testReference();
	testOverlappingReferences();
	testMalformed();

	if (g_failures != 0)
	{
		std::cout << g_failures << " failure(s)\n";
		return 1;
	}

	std::cout << "all blz tests passed\n";
	return 0;
}
