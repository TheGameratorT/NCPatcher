#pragma once

#include <vector>

#include "../utils/types.hpp"

namespace BLZ
{
	/**
	 * @brief Compress module data.
	 *
	 * Produces a complete BLZ image: the backwards-encoded stream followed by
	 * the eight-byte footer uncompressInplace() reads, padded so the footer is
	 * aligned. There is no uncompressed head -- the whole input is encoded --
	 * which is what an overlay wants; an ARM binary keeps its secure area raw
	 * and is not compressed by this tool.
	 *
	 * @param data The data to compress.
	 *
	 * @return The compressed image, or an empty vector when the data does not
	 *         compress to something smaller than itself. Callers must check:
	 *         storing an "compressed" overlay that grew would be worse than not
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
	 * @brief Uncompress module data in-place.
	 * 
	 * @param data_end The pointer to the end of the data to uncompress.
	 */
	void uncompressInplace(u8* data_end);
}
