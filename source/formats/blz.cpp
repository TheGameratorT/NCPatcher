#include "blz.hpp"

#include <algorithm>
#include <span>
#include <stdexcept>

#include "../utils/endian.hpp"

// Backwards LZ, the compression the DS BIOS-adjacent library applies to the ARM
// binaries and to overlays. Both halves of it run from the end of the buffer
// towards the start, which is what lets a module decompress itself in place: the
// read cursor stays ahead of the write cursor, so the tail of the compressed
// image is overwritten only after it has been consumed.
//
// An image is a raw head, the encoded stream, padding, and an eight-byte footer
// giving the decompressor the two ends of the stream and the amount the buffer
// grows by. The head is the part of the input that was left uncompressed, and
// it is what keeps the in-place decode safe: the write cursor descends faster
// than the read cursor, so an image that encoded everything would have the
// output catch up with, and overwrite, stream bytes that had not been read yet.
//
// A token stream is a flag byte followed by up to eight tokens, most significant
// bit first. A clear bit is a literal byte; a set bit is a two-byte reference,
// stored high byte first, holding a length in the top nibble and a distance in
// the remaining twelve bits. Both are biased by three, so a reference covers
// lengths 3..18 at distances 3..4098.

namespace {

constexpr size_t MIN_MATCH = 3;
constexpr size_t MAX_MATCH = MIN_MATCH + 0xF;
constexpr size_t MIN_DISTANCE = 3;
constexpr size_t MAX_DISTANCE = MIN_DISTANCE + 0xFFF;

const char* const SRC_SHORTAGE = "Source shortage.";
const char* const DEST_OVERRUN = "Destination overrun.";
const char* const BAD_FOOTER = "Malformed footer.";

struct Match
{
	size_t length;   // 0 when nothing worth encoding was found
	size_t distance;
};

/**
 * @brief Find the longest reference for the byte about to be encoded.
 *
 * @param src Input data.
 * @param size Size of the input data.
 * @param pos Encoding cursor: src[pos - 1] is the next byte to encode, and
 *            everything from src[pos] up is already encoded.
 *
 * @return The best match, with a length below MIN_MATCH when there is none.
 */
Match findMatch(const u8* src, size_t size, size_t pos)
{
	// A reference may not reach below the cursor, because the decompressor
	// reconstructs the buffer downwards and has nothing there yet. That bounds
	// the length by the distance as well as by the token's own limits -- and,
	// usefully, it means any match of at least MIN_MATCH bytes is also at least
	// MIN_DISTANCE away, so the biased distance can never go negative.
	const size_t maxLength = std::min(pos, MAX_MATCH);
	const size_t maxDistance = std::min(size - pos, MAX_DISTANCE);

	Match best{ 0, 0 };
	if (maxLength < MIN_MATCH)
		return best;

	for (size_t distance = 1; distance <= maxDistance; ++distance)
	{
		if (src[pos + distance - 1] != src[pos - 1])
			continue;

		const size_t limit = std::min(distance, maxLength);
		size_t length = 0;
		while (length < limit && src[pos + distance - 1 - length] == src[pos - 1 - length])
			++length;

		if (length > best.length)
		{
			best = { length, distance };

			// Nothing further out can beat a match that is already as long as
			// the format allows, and the search is quadratic without this.
			if (length == maxLength)
				break;
		}
	}

	return best;
}

/**
 * @brief Where to split the input between its raw head and the encoded stream.
 */
struct Split
{
	size_t in;   // bytes of input left uncompressed at the head
	size_t out;  // index of the first stream byte in the work buffer
};

/**
 * @brief Compress module data.
 *
 * @param src Pointer to input data begin.
 * @param size Size of the input data.
 * @param dst Pointer to a buffer of @p size bytes, filled from the top down.
 *
 * @return The split to build the image around: the stream is dst[out, size) and
 *         it reconstructs src[in, size). A split of { size, size } means nothing
 *         was worth encoding.
 */
Split compressBackward(const u8* src, size_t size, u8* dst)
{
	size_t in = size;   // src[in - 1] is the next byte to encode
	size_t out = size;  // dst[out - 1] is the next byte to write

	// Encoding runs from the end of the input towards the start, so stopping
	// early is just a matter of keeping the stream written so far and leaving
	// the rest of the input raw -- references only ever point at higher
	// addresses, so the tokens already emitted stay valid.
	//
	// (in - out) is how much the image would shrink by stopping here, and it is
	// also the decode's safety margin: at this point the decompressor has
	// written down to `in` and has read down to the stream byte at `out`.
	// Splitting where that difference is at its lowest therefore gives both the
	// smallest image and, because no earlier point in the decode is any tighter,
	// a write cursor that stays clear of the unread stream throughout.
	Split best{ size, size };
	ptrdiff_t bestMargin = 0;

	while (in > 0)
	{
		if (out < 1)
			break;

		// The flag byte precedes its tokens in the stream, so it is written
		// first: writing downwards puts it above them, where the decompressor
		// reads it before the tokens it describes.
		const size_t flagPos = --out;
		u8 flags = 0;
		bool full = false;

		for (int bit = 0; bit < 8; ++bit)
		{
			// Shifting on every pass, taken or not, leaves a stream that ends
			// mid-byte aligned to the top of the flag byte, which is the end
			// the decompressor reads from. It is also what lets the image stop
			// mid-group: the bits below the split describe tokens that are not
			// in the image, and the decompressor stops before reading them.
			flags = u8(flags << 1);
			if (in == 0 || full)
				continue;

			const Match match = findMatch(src, size, in);
			if (match.length < MIN_MATCH)
			{
				if (out < 1)
				{
					full = true;
					continue;
				}
				dst[--out] = src[--in];
			}
			else
			{
				if (out < 2)
				{
					full = true;
					continue;
				}
				in -= match.length;
				const u32 token = u32((match.length - MIN_MATCH) << 12)
				                | u32(match.distance - MIN_DISTANCE);
				dst[--out] = u8(token >> 8);
				dst[--out] = u8(token);
				flags |= 1;
			}

			const ptrdiff_t margin = ptrdiff_t(in) - ptrdiff_t(out);
			if (margin < bestMargin)
			{
				bestMargin = margin;
				best = { in, out };
			}
		}

		dst[flagPos] = flags;

		// The buffer only runs out on data that is not compressing anyway; the
		// best split so far is still a valid image, and is what the caller
		// weighs against leaving the data alone.
		if (full)
			break;
	}

	return best;
}

/**
 * @brief Uncompress module data in place.
 *
 * @param data Pointer to the image begin.
 * @param dataSize Size of the image, footer included.
 * @param bufferSize Size of the buffer holding it, which the decompressed data
 *                   has to fit in.
 */
void UncompressBackward(u8* data, size_t dataSize, size_t bufferSize)
{
	if (dataSize < 8 || dataSize > bufferSize)
		throw std::runtime_error(BAD_FOOTER);

	u8* const bottom = data + dataSize;

	// The footer is read a byte at a time, the way the SDK's own decompressor
	// reads it: the words are little-endian whatever the host is, and reading
	// them through a u32* would additionally be an aliasing bet the optimiser
	// is free to call.
	const std::span<const u8> footer(bottom - 8, 8);
	const u32 offsetIn    = ncp::le::readU32(footer, 0);
	const u32 offsetOut   = ncp::le::readU32(footer, 4);
	const u32 offsetInBtm = offsetIn >> 24;
	const u32 offsetInTop = offsetIn & 0xFFFFFF;

	// The same three checks the SDK makes, which is as much as the footer can
	// be held to before the stream is walked: the stream lies inside the image,
	// its near end is the footer plus at most three bytes of padding, and the
	// decompressed data fits the buffer. Together they are what puts every
	// pointer below inside the buffer, so the loop can bound itself by
	// subtracting pointers rather than by forming out-of-range ones.
	if (offsetInTop > dataSize || offsetInTop < offsetInBtm)
		throw std::runtime_error(BAD_FOOTER);

	if (offsetInBtm < 8 || offsetInBtm > 11)
		throw std::runtime_error(BAD_FOOTER);

	if (offsetOut > bufferSize - dataSize)
		throw std::runtime_error(BAD_FOOTER);

	u8* pOutEnd = bottom + offsetOut;
	u8* pOut    = pOutEnd;
	u8* pInBtm  = bottom - offsetInBtm;
	u8* pInTop  = bottom - offsetInTop;

	// Every bound below is a subtraction between two pointers that are already
	// known to point into the buffer. Forming the out-of-range pointer first
	// and comparing afterwards -- pInBtm - 2 < pInTop, pOut + offset -- is
	// undefined, and an optimiser is entitled to assume it never happens and
	// drop the check, which is exactly what a release build was observed doing.
	while (pInTop < pInBtm)
	{
		u8 flag = *--pInBtm;

		for (int i = 0; i < 8; ++i)
		{
			if (pInBtm <= pInTop)
				throw std::runtime_error(SRC_SHORTAGE);

			if (pOut <= pInTop)
				throw std::runtime_error(DEST_OVERRUN);

			if (!(flag & 0x80))
			{
				*--pOut = *--pInBtm;
			}
			else
			{
				if (pInBtm - pInTop < 2)
					throw std::runtime_error(SRC_SHORTAGE);

				const u32 head = *--pInBtm;
				const size_t offset = (((head & 0xF) << 8) | *--pInBtm) + MIN_DISTANCE;
				const size_t length = (head >> 4) + MIN_MATCH;

				// The reference reads from the part of the buffer that has
				// already been reconstructed, which is what is above pOut.
				if (offset > size_t(pOutEnd - pOut))
					throw std::runtime_error(SRC_SHORTAGE);

				if (length > size_t(pOut - pInTop))
					throw std::runtime_error(DEST_OVERRUN);

				u8* pTmp = pOut + offset;
				for (size_t j = 0; j < length; ++j)
					*--pOut = *--pTmp;
			}

			if (pInBtm <= pInTop)
				break;

			flag = u8(flag << 1);
		}
	}
}

}

namespace BLZ
{
	std::vector<u8> compress(const std::vector<u8>& data)
	{
		const size_t dataSize = data.size();
		if (dataSize < 16)
			return {};

		std::vector<u8> work(dataSize);
		const Split split = compressBackward(data.data(), dataSize, work.data());

		const size_t rawSize = split.in;                 // left uncompressed
		const size_t streamSize = dataSize - split.out;  // encoded, top down

		// The footer is two little-endian words, and the decompressor reads
		// them as aligned words counted back from the end of the image, so the
		// image length has to be a multiple of four. Any slack goes between the
		// stream and the footer, where the decompressor never looks.
		size_t total = rawSize + streamSize + 8;
		const size_t padding = (4 - (total % 4)) % 4;
		total += padding;

		if (total >= dataSize)
			return {};

		std::vector<u8> out(total, 0);
		std::copy_n(data.begin(), std::ptrdiff_t(rawSize), out.begin());
		std::copy(work.begin() + std::ptrdiff_t(split.out), work.end(),
			out.begin() + std::ptrdiff_t(rawSize));

		// Top 8 bits: how far back from the end the encoded stream ends.
		// Low 24 bits: how far back from the end it begins, which is where the
		// raw head stops.
		const u32 offsetIn = u32(total - rawSize) | (u32(8 + padding) << 24);
		const u32 offsetOut = u32(dataSize - total);

		ncp::le::writeU32(out, total - 8, offsetIn);
		ncp::le::writeU32(out, total - 4, offsetOut);

		return out;
	}

	std::vector<u8> uncompress(const std::vector<u8>& data)
	{
		const size_t dataSize = data.size();
		if (dataSize < 8)
			throw std::runtime_error(BAD_FOOTER);

		const u32 destSize = u32(dataSize) + ncp::le::readU32(data, dataSize - 4);

		std::vector<u8> dest(destSize);
		std::copy(data.begin(), data.end(), dest.begin());

		UncompressBackward(dest.data(), dataSize, dest.size());

		return dest;
	}

	void uncompressInplace(std::vector<u8>& data)
	{
		const size_t dataSize = data.size();
		if (dataSize < 8)
			throw std::runtime_error(BAD_FOOTER);

		const u32 destSize = u32(dataSize) + ncp::le::readU32(data, dataSize - 4);
		data.resize(destSize);

		UncompressBackward(data.data(), dataSize, data.size());
	}

	void uncompressInplace(u8* data, size_t dataSize, size_t bufferSize)
	{
		UncompressBackward(data, dataSize, bufferSize);
	}
}
