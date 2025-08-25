#pragma once

#include <vector>

#include "../utils/types.hpp"

namespace BLZ
{
	/**
	 * @brief Compress module data.
	 * 
	 * @param data The data to compress.
	 * 
	 * @return The compressed data.
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
