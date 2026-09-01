#include "lz.hpp"

#include <algorithm>

#include "../system/except.hpp"

namespace ncp::lz {

namespace {

constexpr u8 TYPE_LZ = 0x1;

// A 0x10 token: four bits of length biased by 3, twelve bits of distance
// biased by 1.
constexpr std::size_t MIN_MATCH = 3;
constexpr std::size_t MAX_MATCH = MIN_MATCH + 0xF;
constexpr std::size_t MAX_DISTANCE = 0x1000;

// Never one. The console reads VRAM in 2-byte units, so a reference that
// overlaps the byte being written is unsafe for anything decoded straight into
// it, and the SDK's own encoder therefore starts its search two bytes back. A
// Nitro archive is unpacked into main RAM, where either would work, but refusing
// distance 1 costs a fraction of a percent and produces a stream every decoder
// on the console accepts.
constexpr std::size_t MIN_DISTANCE = 2;

// Match search. A window scan is O(n * 4096) and a 200 KiB archive is a real
// size for one of these, so positions are chained by the hash of the three bytes
// they start, and only the nearest few are measured.
constexpr std::size_t HASH_BITS = 15;
constexpr std::size_t HASH_SIZE = std::size_t(1) << HASH_BITS;
constexpr std::size_t MAX_CHAIN = 256;

[[nodiscard]] std::size_t hash3(const u8* p)
{
	return ((std::size_t(p[0]) << 10) ^ (std::size_t(p[1]) << 5) ^ std::size_t(p[2]))
		& (HASH_SIZE - 1);
}

struct Match
{
	std::size_t length = 0;
	std::size_t distance = 0;
};

class Encoder
{
public:
	explicit Encoder(std::span<const u8> data)
		: m_data(data)
		, m_head(HASH_SIZE, -1)
		, m_prev(data.size(), -1)
	{}

	// The longest reference for the byte at `pos`, or a zero-length match when
	// nothing worth encoding reaches it.
	[[nodiscard]] Match find(std::size_t pos) const
	{
		Match best;
		const std::size_t remaining = m_data.size() - pos;
		if (remaining < MIN_MATCH || pos < MIN_DISTANCE)
			return best;

		const std::size_t maxLength = std::min(remaining, MAX_MATCH);

		// The chain runs from the most recent position backwards, so distances
		// only grow as it is walked. Improving strictly keeps the nearest of
		// two equal-length matches, which is both smaller to encode in other
		// formats and, here, simply stable.
		int candidate = m_head[hash3(&m_data[pos])];
		for (std::size_t tried = 0; candidate >= 0 && tried < MAX_CHAIN; tried++)
		{
			const std::size_t start = std::size_t(candidate);
			const std::size_t distance = pos - start;
			if (distance > MAX_DISTANCE)
				break;

			if (distance >= MIN_DISTANCE)
			{
				std::size_t length = 0;
				while (length < maxLength && m_data[start + length] == m_data[pos + length])
					length++;

				if (length > best.length)
				{
					best.length = length;
					best.distance = distance;
					if (length == maxLength)
						break;
				}
			}

			candidate = m_prev[start];
		}

		if (best.length < MIN_MATCH)
			return Match{};
		return best;
	}

	// Every position has to be recorded, including the ones a match stepped
	// over: a run skipped now is still the best reference for something later.
	void insert(std::size_t pos)
	{
		if (pos + MIN_MATCH > m_data.size())
			return;
		const std::size_t bucket = hash3(&m_data[pos]);
		m_prev[pos] = m_head[bucket];
		m_head[bucket] = int(pos);
	}

private:
	std::span<const u8> m_data;
	std::vector<int> m_head;
	std::vector<int> m_prev;
};

} // namespace

std::optional<Variant> headerVariant(std::span<const u8> data)
{
	if (data.size() < 4)
		return std::nullopt;
	if ((data[0] >> 4) != TYPE_LZ)
		return std::nullopt;

	switch (data[0] & 0x0F)
	{
	case 0x0: return Variant::Lz10;
	case 0x1: return Variant::Lz11;
	default:  return std::nullopt;
	}
}

u32 declaredSize(std::span<const u8> data)
{
	if (!headerVariant(data))
		return 0;
	return u32(data[1]) | (u32(data[2]) << 8) | (u32(data[3]) << 16);
}

std::vector<u8> decompress(std::span<const u8> data, std::size_t limit)
{
	const std::optional<Variant> variant = headerVariant(data);
	if (!variant)
		throw ncp::exception("Not LZ-compressed data: no Nitro compression header.");

	const std::size_t declared = declaredSize(data);
	const std::size_t wanted = limit == 0 ? declared : std::min(limit, declared);

	std::vector<u8> out;
	out.reserve(wanted);

	std::size_t read = 4;
	auto next = [&]() -> u8 {
		if (read >= data.size())
			throw ncp::exception("Truncated LZ stream: it ends before the declared size is reached.");
		return data[read++];
	};

	while (out.size() < wanted)
	{
		const u8 flags = next();
		for (int bit = 0; bit < 8 && out.size() < wanted; bit++)
		{
			if ((flags & (0x80 >> bit)) == 0)
			{
				out.push_back(next());
				continue;
			}

			std::size_t length = 0;
			std::size_t distance = 0;
			if (*variant == Variant::Lz10)
			{
				const u8 first = next();
				const u8 second = next();
				length = std::size_t(first >> 4) + MIN_MATCH;
				distance = ((std::size_t(first & 0x0F) << 8) | second) + 1;
			}
			else
			{
				// 0x11 spends the first nibble on which of three token widths
				// this is, which is how it reaches runs of up to 0x10110 bytes.
				const u8 first = next();
				const u8 kind = u8(first >> 4);
				if (kind == 0)
				{
					const u8 second = next();
					const u8 third = next();
					length = ((std::size_t(first & 0x0F) << 4) | (second >> 4)) + 0x11;
					distance = ((std::size_t(second & 0x0F) << 8) | third) + 1;
				}
				else if (kind == 1)
				{
					const u8 second = next();
					const u8 third = next();
					const u8 fourth = next();
					length = ((std::size_t(first & 0x0F) << 12) | (std::size_t(second) << 4)
						| (third >> 4)) + 0x111;
					distance = ((std::size_t(third & 0x0F) << 8) | fourth) + 1;
				}
				else
				{
					const u8 second = next();
					length = std::size_t(kind) + 1;
					distance = ((std::size_t(first & 0x0F) << 8) | second) + 1;
				}
			}

			if (distance > out.size())
			{
				throw ncp::exception("Malformed LZ stream: a reference reaches back before "
					"the start of the data.");
			}

			// Byte at a time and deliberately so: a reference may overlap what
			// it is producing, which is how a run of one repeated byte is
			// encoded, and a block copy would read bytes that do not exist yet.
			for (std::size_t i = 0; i < length && out.size() < wanted; i++)
			{
				const u8 byte = out[out.size() - distance];
				out.push_back(byte);
			}
		}
	}

	return out;
}

std::vector<u8> compress(std::span<const u8> data)
{
	if (data.size() > 0xFFFFFF)
	{
		throw ncp::exception("Cannot LZ-compress " + std::to_string(data.size())
			+ " bytes: the header carries a 24-bit size.");
	}

	std::vector<u8> out;
	out.reserve(data.size() + data.size() / 8 + 8);

	const u32 header = (u32(data.size()) << 8) | (u32(TYPE_LZ) << 4);
	out.push_back(u8(header));
	out.push_back(u8(header >> 8));
	out.push_back(u8(header >> 16));
	out.push_back(u8(header >> 24));

	Encoder encoder(data);
	std::size_t pos = 0;
	while (pos < data.size())
	{
		const std::size_t flagsAt = out.size();
		out.push_back(0);

		u8 flags = 0;
		for (int bit = 0; bit < 8 && pos < data.size(); bit++)
		{
			const Match match = encoder.find(pos);
			if (match.length >= MIN_MATCH)
			{
				flags |= u8(0x80 >> bit);
				const u16 token = u16(((match.length - MIN_MATCH) << 12) | (match.distance - 1));
				out.push_back(u8(token >> 8));
				out.push_back(u8(token & 0xFF));
				for (std::size_t i = 0; i < match.length; i++)
					encoder.insert(pos + i);
				pos += match.length;
			}
			else
			{
				out.push_back(data[pos]);
				encoder.insert(pos);
				pos++;
			}
		}
		out[flagsAt] = flags;
	}

	return out;
}

} // namespace ncp::lz
