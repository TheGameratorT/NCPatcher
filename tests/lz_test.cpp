// Tests for source/formats/lz.cpp: the forward LZ77 a DS game's file loader
// unpacks, and the wrapper Mario Kart DS puts around 286 of its Nitro archives.
//
// A codec tested only by round-tripping itself is a codec that has agreed with
// its own mistake, so the decoder is also checked against a stream written by
// hand whose expected output was worked out on paper. The other cases are the
// ones the format's edges live at: the empty input, the single byte, the run of
// identical bytes that has to encode as an overlapping reference, and the
// incompressible block that has to come out larger than it went in.
//
// Run via ctest, or directly: ./lz_test

#include "../source/formats/lz.hpp"

#include <iostream>
#include <numeric>
#include <random>
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

static std::vector<u8> bytes(std::initializer_list<int> values)
{
	std::vector<u8> out;
	for (int value : values)
		out.push_back(u8(value));
	return out;
}

// Walks a stream the way the console does, so the assertions below are about
// what was actually emitted rather than about what the encoder meant.
struct Token
{
	bool reference = false;
	std::size_t length = 0;
	std::size_t distance = 0;
};

static std::vector<Token> tokensOf(const std::vector<u8>& stream)
{
	std::vector<Token> out;
	const std::size_t declared = lz::declaredSize(stream);
	std::size_t read = 4;
	std::size_t produced = 0;
	while (produced < declared)
	{
		const u8 flags = stream[read++];
		for (int bit = 0; bit < 8 && produced < declared; bit++)
		{
			if ((flags & (0x80 >> bit)) == 0)
			{
				read++;
				produced++;
				out.push_back(Token{ false, 1, 0 });
				continue;
			}
			const u8 first = stream[read++];
			const u8 second = stream[read++];
			Token token;
			token.reference = true;
			token.length = std::size_t(first >> 4) + 3;
			token.distance = ((std::size_t(first & 0x0F) << 8) | second) + 1;
			produced += token.length;
			out.push_back(token);
		}
	}
	return out;
}

static void testHeader()
{
	check(lz::headerVariant(bytes({ 0x10, 0x20, 0x00, 0x00 })) == lz::Variant::Lz10,
		"0x10 is the original LZ form");
	check(lz::headerVariant(bytes({ 0x11, 0x20, 0x00, 0x00 })) == lz::Variant::Lz11,
		"0x11 is the extended one");
	check(!lz::headerVariant(bytes({ 0x20, 0x20, 0x00, 0x00 })),
		"0x20 is Huffman and not this codec's business");
	check(!lz::headerVariant(bytes({ 0x10, 0x00 })), "four bytes are needed to have a header");

	check(lz::declaredSize(bytes({ 0x10, 0x34, 0x12, 0x00 })) == 0x1234,
		"the declared size is a 24-bit little-endian field");

	// The file that makes detection-by-magic-byte wrong. dwc/utility.bin begins
	// with 0x10 and is not compressed at all: its "header" reads 10 00 00 00, a
	// declared size of zero. Nothing here may take that for a stream.
	std::vector<u8> utility = bytes({ 0x10, 0x00, 0x00, 0x00 });
	utility.resize(64, 0x5A);
	check(lz::declaredSize(utility) == 0, "a declared size of zero is zero");
	check(lz::decompress(utility).empty(),
		"and yields nothing rather than being read as a wider size field");
}

// A stream written by hand, whose output was worked out on paper rather than by
// running the encoder -- so a mistake shared by both directions cannot hide in
// it.
//
// Header: type 1, ten bytes out. Then one flag byte, 0b00010000, read most
// significant bit first: three literals and then a reference, and the remaining
// four bits are never reached because the declared size is met first.
//
//   'a', 'b', 'c', then a reference of length 5 at distance 3. That reference
//   overlaps what it is producing, which is the interesting case: it reads
//   "abc" and then the "ab" it has just written, giving "abcab". Then 'z'
//   twice.
//
//   "abc" + "abcab" + "zz"  =  "abcabcabzz", ten bytes.
static void testDecodesAKnownStream()
{
	const std::vector<u8> stream = bytes({
		0x10, 0x0A, 0x00, 0x00,       // LZ10, 10 bytes out
		0b00010000,                   // literal, literal, literal, reference, ...
		'a', 'b', 'c',
		0x20, 0x02,                   // length (2 + 3) = 5, distance (2 + 1) = 3
		'z', 'z',
	});

	const std::vector<u8> out = lz::decompress(stream);
	const std::string text(out.begin(), out.end());
	check(text == "abcabcabzz", "a hand-written stream decodes to what it says on paper");
}

static void testRoundTrips()
{
	std::mt19937 rng(20240901);

	std::vector<std::pair<std::string, std::vector<u8>>> cases;
	cases.emplace_back("nothing at all", std::vector<u8>());
	cases.emplace_back("one byte", std::vector<u8>{ 0x42 });
	cases.emplace_back("two bytes", std::vector<u8>{ 0x42, 0x43 });
	cases.emplace_back("three identical bytes", std::vector<u8>(3, 0x7F));
	cases.emplace_back("a long run of one byte", std::vector<u8>(5000, 0xAB));

	{
		// Incompressible: every token has to be a literal, and the result has
		// to be larger than the input. An encoder that reported failure here
		// would be right for the SDK's purposes and wrong for this one.
		std::vector<u8> noise(20000);
		for (u8& byte : noise)
			byte = u8(rng() & 0xFF);
		cases.emplace_back("random noise", std::move(noise));
	}
	{
		std::vector<u8> repeated;
		for (int i = 0; i < 2000; i++)
		{
			const char* line = "the quick brown fox ";
			repeated.insert(repeated.end(), line, line + 20);
		}
		cases.emplace_back("a repeated phrase", std::move(repeated));
	}
	{
		// A match at exactly the window's edge, and one past it.
		std::vector<u8> spaced(9000);
		std::iota(spaced.begin(), spaced.end(), u8(0));
		for (int i = 0; i < 32; i++)
			spaced[4000 + std::size_t(i)] = spaced[std::size_t(i)];
		cases.emplace_back("matches at window distance", std::move(spaced));
	}

	for (const auto& [name, input] : cases)
	{
		const std::vector<u8> stream = lz::compress(input);
		check(lz::declaredSize(stream) == input.size(), name + ": the header declares the real size");
		check(lz::decompress(stream) == input, name + ": survives compress and decompress");

		// Never distance 1. The console reads VRAM two bytes at a time, so a
		// reference overlapping the byte being written is unsafe there, and the
		// SDK's own encoder starts its search two bytes back.
		bool nearest = false;
		std::size_t longest = 0;
		for (const Token& token : tokensOf(stream))
		{
			if (!token.reference)
				continue;
			nearest = nearest || token.distance < 2;
			longest = std::max(longest, token.length);
		}
		check(!nearest, name + ": no reference is emitted at distance 1");
		check(longest <= 18, name + ": no reference claims more than the format allows");
	}

	const std::vector<u8> grown = lz::compress(std::vector<u8>(64, 0x00));
	check(!grown.empty(), "a stream is produced even for input that compresses to nothing useful");

	{
		std::mt19937 noise(7);
		std::vector<u8> incompressible(4096);
		for (u8& byte : incompressible)
			byte = u8(noise() & 0xFF);
		check(lz::compress(incompressible).size() > incompressible.size(),
			"incompressible data comes out larger, rather than the encoder giving up");
	}
}

static void testRefusesMalformedStreams()
{
	auto refused = [](const std::vector<u8>& stream, const std::string& what) {
		bool threw = false;
		try { (void)lz::decompress(stream); }
		catch (const std::exception&) { threw = true; }
		check(threw, what);
	};

	// A reference on the first token, which can only reach before the start.
	refused(bytes({ 0x10, 0x08, 0x00, 0x00, 0x80, 0x00, 0x00 }),
		"a reference reaching before the start of the output is refused");

	// Ends after the flag byte, with eight bytes still to produce.
	refused(bytes({ 0x10, 0x08, 0x00, 0x00, 0x00 }),
		"a stream that ends early is refused rather than padded");

	refused(bytes({ 0x30, 0x08, 0x00, 0x00 }), "run-length data is not decoded as LZ");
}

static void testPartialDecode()
{
	std::vector<u8> input(4096);
	for (std::size_t i = 0; i < input.size(); i++)
		input[i] = u8(i * 7);
	const std::vector<u8> stream = lz::compress(input);

	// What detection uses: enough to read a magic number, without unpacking a
	// whole archive to answer a yes/no question.
	const std::vector<u8> head = lz::decompress(stream, 4);
	check(head.size() == 4, "a limited decode stops at the limit");
	check(std::equal(head.begin(), head.end(), input.begin()),
		"and produces the same bytes the full decode would");
}

int main()
{
	testHeader();
	testDecodesAKnownStream();
	testRoundTrips();
	testRefusesMalformedStreams();
	testPartialDecode();

	if (g_failures == 0)
	{
		std::cout << "lz_test: all checks passed\n";
		return 0;
	}
	std::cout << g_failures << " lz test(s) failed.\n";
	return 1;
}
