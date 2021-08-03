#include "blz.hpp"

#include <stdexcept>

static const char* SRC_SHORTAGE = "Source shortage.";
static const char* DEST_OVERRUN = "Destination overrun.";

static int CompressBackward_sub2(const u8 *a1, const u8 *a2, int a3)
{
	int i;
	for (i = 0; i < a3 && *a1 == *a2; ++i)
	{
		--a1;
		--a2;
	}
	return i;
}

static int CompressBackward_sub1(const u8 *a1, int a2, const u8 *a3, int a4, int& a5)
{
	u8 v10 = a1[a2 - 1];
	int v7 = 0;
	for (int i = 0; i < a4; ++i)
	{
		if (a3[i] == v10)
		{
			int v6 = i + 1;
			if (i + 1 > a2)
				v6 = a2;
			int v8 = CompressBackward_sub2(&a1[a2 - 1], &a3[i], v6);
			if (v7 < v8)
			{
				v7 = v8;
				a5 = i;
			}
		}
	}
	return v7;
}

/**
 * @brief Compress module data.
 * 
 * @param src Pointer to input data begin.
 * @param size Size of the input data.
 * @param dst Pointer to output data begin.
 * 
 * @return The number of compressed bytes. (in_size - out_size)
 */
static size_t CompressBackward(const void *src, size_t size, void *dst)
{
	const u8* src_ = reinterpret_cast<const u8*>(src);
	u8* dst_ = reinterpret_cast<u8*>(dst);

	size_t v13 = size;
	size_t v12 = size;
	while (v13 > 0)
	{
		if (v12 <= 0)
			return -1;
		int v11 = 0;
		int v10 = --v12;
		for (int i = 0; i <= 7; ++i)
		{
			v11 *= 2;
			if (v13 > 0)
			{
				const u8* v9 = &src_[v13];
				int v8 = size - v13;
				int v2 = v13;
				if (v13 > 18)
					v2 = 18;
				int v6 = v2;
				const u8* v7 = &v9[-v2];
				int v1 = v8;
				if (v8 > 4098)
					v1 = 4098;
				int v5;
				int v4 = CompressBackward_sub1(v7, v6, v9, v1, v5);
				if (v4 <= 2)
				{
					if (v12 <= 0)
						return -1;
					dst_[--v12] = src_[--v13];
				}
				else
				{
					if (v12 <= 1)
					  return -1;
					v13 -= v4;
					v5 -= 2;
					u16 v3 = v5 & 0xFFF | (((u16)v4 - 3) << 12);
					--v12;
					dst_[v12--] = v3 >> 8;
					dst_[v12] = v3;
					v11 |= 1;
				}
			}
		}
		dst_[v10] = v11;
	}
	return v12;
}

/**
 * @brief Uncompress module data.
 * 
 * @param bottom Pointer to input data end.
 */
static void UncompressBackward(void* bottom)
{
	u32 offsetOut   = *(reinterpret_cast<u32*>(bottom) - 1);
	u32 offsetIn    = *(reinterpret_cast<u32*>(bottom) - 2);
	u32 offsetInBtm = offsetIn >> 24;
	u32 offsetInTop = offsetIn & 0xFFFFFF;

	u8* pOut   = reinterpret_cast<u8*>(bottom) + offsetOut;
	u8* pInBtm = reinterpret_cast<u8*>(bottom) - offsetInBtm;
	u8* pInTop = reinterpret_cast<u8*>(bottom) - offsetInTop;

	while (pInTop < pInBtm)
	{
		u8 flag = *--pInBtm;

		for (int i = 0; i < 8; ++i)
		{
			if (pInBtm < pInTop)
				throw std::runtime_error(SRC_SHORTAGE);

			if (pOut < pInTop)
				throw std::runtime_error(DEST_OVERRUN);

			if (!(flag & 0x80))
			{
				*--pOut = *--pInBtm;
			}
			else
			{
				if (pInBtm - 2 < pInTop)
					throw std::runtime_error(DEST_OVERRUN);

				u32 length = *--pInBtm;
				u32 offset = (((length & 0xF) << 8) | (*--pInBtm)) + 3;
				length = (length >> 4) + 3;

				u8* pTmp = pOut + offset;

				if (pOut - length < pInTop)
					throw std::runtime_error(DEST_OVERRUN);

				for (int j = 0; j < length; ++j)
					*--pOut = *--pTmp;
			}

			if (pInBtm <= pInTop)
				break;

			flag <<= 1;
		}
	}
}

namespace BLZ
{
	std::vector<u8> compress(const std::vector<u8>& data)
	{
		size_t dataSize = data.size();
		std::vector<u8> dest(dataSize);

		int destSize = CompressBackward(data.data(), dataSize, dest.data());
		if (destSize == -1)
			throw std::runtime_error("Compression failed.");

		dest.resize(destSize);
		return dest;
	}

	std::vector<u8> uncompress(const std::vector<u8>& data)
	{
		size_t dataSize = data.size();
		u32 destSize = dataSize + *reinterpret_cast<const u32*>(&data[dataSize - 4]);

		std::vector<u8> dest(destSize);
		std::copy(data.begin(), data.end(), dest.begin());
		
		UncompressBackward(&dest[dataSize]);

		return dest;
	}

	void uncompress(std::vector<u8>& data)
	{
		size_t dataSize = data.size();
		u32 destSize = dataSize + *reinterpret_cast<u32*>(&data[dataSize - 4]);
		data.resize(destSize);
		
		UncompressBackward(&data[dataSize]);
	}

	void uncompress(u8* data_end)
	{
		UncompressBackward(data_end);
	}
}
