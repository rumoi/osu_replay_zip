#pragma once

// CRC32C: the Castagnoli polynomial, bits reflected, no inversion in or out, as the SSE 4.2 crc32 instruction computes it

#include <array>
#include <cstddef>
#include <cstring>

#include "orz_types.h"

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#define ORZ_CRC32C_SSE42
#include <nmmintrin.h>
#elif defined(__ARM_FEATURE_CRC32)
#define ORZ_CRC32C_ARM
#include <arm_acle.h>
#endif

namespace orz {

#if defined(ORZ_CRC32C_SSE42)

#if defined(_MSC_VER)
#define TARGET_SSE42
#else
#define TARGET_SSE42 __attribute__((target("sse4.2"))) // the CRC instructions, without building everything for SSE 4.2
#endif

TARGET_SSE42 inline u32 crc32c(u32 crc, const void* data, size_t size) {

	const u8* p{ (const u8*)data };

#if defined(_M_X64) || defined(__x86_64__)

	u64 c{ crc };

	for (; size >= 8; size -= 8, p += 8) {

		u64 v;

		memcpy(&v, p, 8);

		c = _mm_crc32_u64(c, v);

	}

	u32 c32{ (u32)c };

#else

	u32 c32{ crc };

	for (; size >= 4; size -= 4, p += 4) {

		u32 v;

		memcpy(&v, p, 4);

		c32 = _mm_crc32_u32(c32, v);

	}

#endif

	for (; size; --size)
		c32 = _mm_crc32_u8(c32, *p++);

	return c32;
}

#elif defined(ORZ_CRC32C_ARM)

inline u32 crc32c(u32 crc, const void* data, size_t size) {

	const u8* p{ (const u8*)data };

	for (; size >= 8; size -= 8, p += 8) {

		u64 v;

		memcpy(&v, p, 8);

		crc = __crc32cd(crc, v);

	}

	for (; size; --size)
		crc = __crc32cb(crc, *p++);

	return crc;
}

#else

// a byte at a time from a table, where there are no CRC instructions
inline u32 crc32c(u32 crc, const void* data, size_t size) {

	static constexpr auto table{ [] {

		std::array<u32, 256> t{};

		for (u32 i{}; i < 256; ++i) {

			u32 c{ i };

			for (int k{}; k < 8; ++k)
				c = (c >> 1) ^ (0x82F63B78 & (0u - (c & 1)));

			t[i] = c;

		}

		return t;
	}() };

	const u8* p{ (const u8*)data };

	for (; size; --size)
		crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);

	return crc;
}

#endif

}
