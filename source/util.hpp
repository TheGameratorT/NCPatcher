#pragma once

#include <cstring>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <array>
#include <chrono>

namespace Util {

constexpr size_t indexOf(const char* val, const char* vals[], size_t size) noexcept
{
	for (size_t index = 0; index < size; index++)
		if (strcmp(val, vals[index]) == 0)
			return index;
	return -1;
}

template <class T1, class T2>
constexpr size_t indexOf(const T1 val, const T2* vals, size_t size) noexcept
{
	for (size_t index = 0; index < size; index++)
		if (val == vals[index])
			return index;
	return -1;
}

static inline std::string strRepl(std::string str, char chr, char new_chr)
{
	std::replace(str.begin(), str.end(), chr, new_chr);
	return str;
}

template<class... Args>
constexpr std::string concat(std::size_t preAllocSz, const Args&... args)
{
	std::string result;
	result.reserve(preAllocSz);
	for (auto s : {std::string_view(args)...})
		result += s;
	return result;
}

template<typename T>
inline void write(void* address, T value)
{
	std::memcpy(address, &value, sizeof(T));
}

template<typename T>
inline T read(const void* address)
{
	T value;
	std::memcpy(&value, address, sizeof(T));
	return value;
}

/*template<typename T>
class MemVarHandler
{
public:
	explicit MemVarHandler(T value, void* address) :
		m_value(value), m_address(address)
	{}

	MemVarHandler& operator=(T value)
	{
		std::memcpy(address, &value, sizeof(T));
	    return *this;
	}

private:
	T m_value;
	void* m_address;
};

template<typename T>
inline MemVarHandler<T> get(void* address)
{
	T value;
	std::memcpy(value, address, sizeof(T));
	return MemVarHandler<T>(value, address);
}*/

template <typename TP>
constexpr std::time_t toTimeT(TP tp)
{
	using namespace std::chrono;
	auto sctp = time_point_cast<system_clock::duration>(tp - TP::clock::now() + system_clock::now());
	return system_clock::to_time_t(sctp);
}

template <typename T>
constexpr bool overlaps(T x1, T x2, T y1, T y2)
{
	return x2 > y1 && y2 > x1;
}

int addrToInt(const std::string& in);
std::string intToAddr(int in, int align, bool prefix = true);

void printDataAsHex(const void* data, std::size_t size, std::size_t rowlen);

}
