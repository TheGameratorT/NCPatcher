#pragma once

// Forward LZ77: the compression a DS game's own file loader unpacks.
//
// Not the BLZ next door. That one is the *backwards* variant, used for the ARM
// binaries and the overlays so a module can decompress itself in place with the
// read cursor staying ahead of the write cursor. This one runs front to back and
// wraps ordinary data files. Mario Kart DS stores 286 of its Nitro archives this
// way, named `.carc`, and what is inside the wrapper is an ordinary NARC.
//
// Two forms share the header. `0x10` is the original, with 4-bit lengths and
// 12-bit distances, and is what the Nitro SDK emits. `0x11` is a later extension
// with longer runs, which the console decodes and the SDK never writes. Both are
// read here, because reading a format costs little and refusing a valid file is
// a real failure; only `0x10` is written, because round-tripping a container
// that arrived compressed is the one thing that needs an encoder, and nothing
// needs NCPatcher to invent an LZ11 stream.
//
// The format, per GBATEK's LZ77 section and confirmed against the console's own
// decoders:
//
//   - A four-byte little-endian header: a 4-bit parameter, a 4-bit type (1 for
//     LZ) and a 24-bit decompressed size. Byte 0 is therefore 0x10 or 0x11.
//   - Then a flag byte followed by up to eight tokens, most significant bit
//     first.
//   - A clear bit is one literal byte.
//   - A set bit is a back reference. For 0x10 it is a big-endian u16: four bits
//     of length, biased by 3, then twelve bits of distance, biased by 1. For
//     0x11 the first nibble picks between a two-, three- and four-byte form,
//     which is what buys the longer runs.
//   - Decoding stops once the declared number of bytes has been produced.
//
// A declared size of zero means exactly that: nothing to decompress. It is not
// an escape to a wider size field -- the console reads the size and stops -- and
// treating it as one is how `dwc/utility.bin`, which merely begins with 0x10,
// gets mistaken for compressed data.

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "../utils/types.hpp"

namespace ncp::lz {

enum class Variant
{
	Lz10,
	Lz11
};

// The variant this data's header names, or nothing when it carries no LZ header
// at all.
//
// A header on its own proves very little: a file whose first byte happens to be
// 0x10 is not thereby compressed. This is a filter, never an answer. Deciding
// whether something really is a compressed archive means looking at what comes
// out; see rom::narcWrapper.
[[nodiscard]] std::optional<Variant> headerVariant(std::span<const u8> data);

// The decompressed size the header declares. Zero when there is no header.
[[nodiscard]] u32 declaredSize(std::span<const u8> data);

// Throws when the stream is malformed: a token that runs past the end of the
// input, or a reference reaching back before the start of the output.
//
// `limit` stops once that many bytes have been produced, for a caller that only
// wants to look at the head of the data and should not pay to unpack a 200 KiB
// archive to do it. Zero means the whole thing.
[[nodiscard]] std::vector<u8> decompress(std::span<const u8> data, std::size_t limit = 0);

// LZ10, always. This produces a stream even when the stream is larger than what
// went in, which the console's own compressor refuses to do -- and rightly, for
// its purpose, which is deciding whether compressing is worth it. That is not
// the question here. A game that reads an archive through its decompressor will
// not accept a raw one in its place, so a container that arrived compressed goes
// back compressed whatever that costs; Mario Kart's own GeneralMenu_es.carc is
// 85 bytes stored for 72 raw, and the game loads it.
[[nodiscard]] std::vector<u8> compress(std::span<const u8> data);

} // namespace ncp::lz
