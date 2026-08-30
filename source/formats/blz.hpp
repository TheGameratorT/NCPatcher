#pragma once

#include <vector>

#include "../utils/types.hpp"

namespace BLZ
{
	/**
	 * @brief Compress module data.
	 *
	 * Produces a complete BLZ image: an uncompressed head, the backwards-encoded
	 * stream, and the eight-byte footer uncompressInplace() reads, padded so the
	 * footer is aligned. The head is the part of the input the encoder stopped
	 * short of, and it is not optional: decompression runs in place, so without
	 * it the output cursor catches up with the stream and overwrites bytes that
	 * have not been read yet.
	 *
	 * @param data The data to compress.
	 *
	 * @return The compressed image, or an empty vector when the data does not
	 *         compress to something smaller than itself. Callers must check:
	 *         storing a "compressed" overlay that grew would be worse than not
	 *         compressing it at all.
	 */
	std::vector<u8> compress(const std::vector<u8>& data);

	/**
	 * @brief Uncompress module data.
	 * 
	 * @param data The data to uncompress.
	 * 
	 * @return The decompressed data.
	 */
	std::vector<u8> uncompress(const std::vector<u8>& data);

	/**
	 * @brief Uncompress module data in-place.
	 * 
	 * @param data The data to uncompress.
	 */
	void uncompressInplace(std::vector<u8>& data);

	/**
	 * @brief Uncompress module data in-place, within a larger buffer.
	 *
	 * For a module whose compressed image is only part of what it was loaded
	 * into -- an ARM binary, whose image ends at compStaticEnd rather than at
	 * the end of the file.
	 *
	 * @param data The pointer to the beginning of the image.
	 * @param dataSize The size of the image, footer included.
	 * @param bufferSize The size of the buffer the image sits in, which the
	 *                   decompressed data has to fit in.
	 */
	void uncompressInplace(u8* data, size_t dataSize, size_t bufferSize);
}
