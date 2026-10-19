#pragma once

#include <bit>
#include <charconv>
#include <cmath>

namespace orz {

namespace orz_decimal {

	// (q + rest) * 2^e2 to the nearest float, ties to even; sticky = rest > 0 (q then needs 25+ bits)
	FORCEINLINE float round_to_float(u64 q, int e2, bool sticky) {

		const int bits{ (int)std::bit_width(q) };

		if (bits <= 24)
			return std::ldexp((float)q, e2); // exact

		const int shift{ bits - 24 };

		u64 m{ q >> shift };

		const u64 rest{ q & ((u64(1) << shift) - 1) };
		const u64 half{ u64(1) << (shift - 1) };

		if (rest > half || (rest == half && (sticky || (m & 1))))
			++m; // can reach 2^24, still exact

		return std::ldexp((float)m, e2 + shift);
	}

	constexpr u64 POW10[20]{
		1ull, 10ull, 100ull, 1000ull, 10000ull, 100000ull, 1000000ull, 10000000ull,
		100000000ull, 1000000000ull, 10000000000ull, 100000000000ull, 1000000000000ull, 10000000000000ull,
		100000000000000ull, 1000000000000000ull, 10000000000000000ull, 100000000000000000ull,
		1000000000000000000ull, 10000000000000000000ull
	};

	// m * 10^k, false when 64 bits are not enough
	FORCEINLINE bool to_float_integer(i64 m, int k, float& out) {

		const bool neg{ m < 0 };
		const u64 a{ neg ? 0ull - (u64)m : (u64)m };

		if (a == 0) {

			out = 0.f;

			return true;
		}

		float v;

		if (k >= 0) {

			if (k > 19 || a > UINT64_MAX / POW10[k])
				return false;

			v = round_to_float(a * POW10[k], 0, false);

		} else {

			// (a * 2^s) / 10^-k with a * 2^s filling 64 bits
			if (-k > 19)
				return false;

			const u64 d{ POW10[-k] };
			const int s{ 64 - (int)std::bit_width(a) };

			const u64 n{ a << s };
			const u64 q{ n / d };
			const u64 r{ n % d };

			if (q < (u64(1) << 25))
				return false; // too few bits to round from

			v = round_to_float(q, -s, r != 0);

		}

		out = neg ? -v : v;

		return true;
	}

	// m * 10^k as the nearest float
	inline float to_float(i64 m, int k) {

		if (float v; to_float_integer(m, k, v))
			return v;

		char b[48];

		char* p{ std::to_chars(b, b + 24, m).ptr };

		*p++ = 'E';
		p = std::to_chars(p, b + sizeof b, k).ptr;

		float v{};

		std::from_chars(b, p, v);

		return v;
	}

}

}
