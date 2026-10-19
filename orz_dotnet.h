#pragma once

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "orz_decimal.h"

namespace orz {

namespace dotnet {

	inline auto digits7_text(const double v) {

		struct _digits {
			i64 m;
			int e;
		};

		char b[64];

		*std::to_chars(b, b + sizeof(b) - 1, v, std::chars_format::scientific, 40).ptr = 0;

		const char* ptr{ b };

		const bool neg{ *ptr == '-' };

		ptr += neg;

		auto m{ (i64)ptr[0] - '0' };

		for (int i{ 2 }; i < 8; ++i)
			m = m * 10 + (ptr[i] - '0');

		int e{ atoi(strchr(ptr, 'e') + 1) };

		if (ptr[8] >= '5' && ++m == 10000000) {

			m = 1000000;
			++e;

		}

		return _digits{ neg ? -m : m, e };
	}

	// G7 digits: v ~= m * 10^(e - 6), |m| in [10^6, 10^7), ties away from zero; { 0, 0 } for 0 and non-finite
	FORCEINLINE auto digits7(const double v) {

		struct _digits {
			i64 m;
			int e;
		};

		if (v == 0.0 || !std::isfinite(v))
			return _digits{ 0, 0 };

		// floats: 2 v = M * 2^s exactly, t = floor(2 v / 10^(e - 6)) holds the 7 digits and the rounding bit
		if (const float f{ (float)v }; (double)f == v) {

			const u32 bits{ std::bit_cast<u32>(f) };
			const u32 field{ (bits >> 23) & 0xff };
			const u64 M{ field ? (bits & 0x7fffff) | 0x800000 : bits & 0x7fffff };
			const int s{ (field ? (int)field - 150 : -149) + 1 };

			int e{ (int)std::floor(std::log10(std::abs(v))) }; // corrected below if off by one

			for (int tries{}; tries < 3; ++tries) {

				const int k{ e - 6 };

				u64 num{ M }, den{ 1 };

				if (s >= 0) {

					if (s > 39)
						break;

					num <<= s;

				} else {

					if (s < -62)
						break;

					den <<= -s;

				}

				if (k >= 0) {

					if (k > 19 || den > ~0ull / orz_decimal::POW10[k])
						break;

					den *= orz_decimal::POW10[k];

				} else {

					if (-k > 19 || num > ~0ull / orz_decimal::POW10[-k])
						break;

					num *= orz_decimal::POW10[-k];

				}

				const u64 t{ num / den };

				if (t >= 20000000) {

					++e;

					continue;
				}

				if (t < 2000000) {

					--e;

					continue;
				}

				i64 m{ (i64)(t >> 1) };

				if ((t & 1) && ++m == 10000000) {

					m = 1000000;
					++e;

				}

				return _digits{ bits >> 31 ? -m : m, e };
			}

		}

		const auto [m, e] { digits7_text(v) };

		return _digits{ m, e };
	}

	inline double pow10i(int k) {

		static const double t[]{
			1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
			1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22
		};

		return (k >= 0 && k <= 22) ? t[k] : std::pow(10.0, k);
	}

	// the floats that print as a given text: G7 an interval, shortest text one bit pattern
	struct text_bounds {

		double lo{}, hi{};
		bool neg{}, zero{};
		bool exact{};
		u32 bits{};

		text_bounds() = default;

		text_bounds(double target, bool exact_text) {

			if (exact_text) {

				exact = true;
				bits = std::bit_cast<u32>((float)target);

				return;
			}

			const auto [m, e] { digits7(target) };

			if (m == 0) {

				zero = true;

				return;
			}

			neg = m < 0;

			const i64 am{ m < 0 ? -m : m };

			const auto scaled = [](i64 num, int k) {

				return k >= 0 ? (double)num / (2.0 * pow10i(k)) : (double)num * (pow10i(-k) / 2.0);
			};

			const int k{ 6 - e };

			lo = am == 1000000 ? scaled(2 * 9999999 + 1, k + 1) : scaled(2 * am - 1, k);
			hi = scaled(2 * am + 1, k);

		}

		bool contains(float v) const {

			if (exact)
				return (std::bit_cast<u32>(v) == bits);

			if (zero)
				return (v == 0.f);

			if (v == 0.f || (v < 0.f) != neg)
				return false;

			const double a{ std::abs((double)v) };

			return a >= lo && a < hi;
		}

		// -1 below the accepted floats, 0 among them, +1 above them
		int side(float v) const {

			if (exact) { // floats in order, -0 just below +0

				const auto key = [](u32 b) {

					return (b >> 31) ? -(i64)(b & 0x7fffffff) - 1 : (i64)b;
				};

				const i64 kv{ key(std::bit_cast<u32>(v)) }, kt{ key(bits) };

				return kv < kt ? -1 :
					(kv > kt ? 1 : 0);
			}

			if (zero)
				return v == 0.f ? 0 :
					(v < 0.f ? -1 : 1);

			if (!neg) {

				if (v <= 0.f)
					return -1;

				const double a{ (double)v };

				return a < lo ? -1 :
					(a >= hi ? 1 : 0);
			}
			
			if (v >= 0.f)
				return 1;

			const double a{ -(double)v };

			return a < lo ? 1 :
				(a >= hi ? -1 : 0);
		}
	};

	inline bool same_text(float a, float b, bool exact_text) {
		return text_bounds{ b, exact_text }.contains(a);
	}

	inline char* layout_digits(const char* d, int n, int e, char* out) {

		if (e <= -5 || e >= 7) {

			*out++ = d[0];

			if (n > 1) {

				*out++ = '.';

				for (int i{ 1 }; i < n; ++i)
					*out++ = d[i];

			}

			out += sprintf(out, "E%c%02d", e < 0 ? '-' : '+', e < 0 ? -e : e);

			return out;
		}

		if (e < 0) {

			*out++ = '0';
			*out++ = '.';

			for (int i{}; i < -e - 1; ++i)
				*out++ = '0';

			for (int i{}; i < n; ++i)
				*out++ = d[i];

			return out;
		}

		for (int i{}; i <= e; ++i)
			*out++ = i < n ? d[i] : '0';

		if (n > e + 1) {

			*out++ = '.';

			for (int i{ e + 1 }; i < n; ++i)
				*out++ = d[i];

		}

		return out;
	}

	// osu!stables 7 significant digits
	inline char* to_string_g7(float v, char* out) {

		auto [m, e] { digits7(v) };

		if (m == 0) {

			*out++ = '0';

			return out;
		}

		if (m < 0) {

			*out++ = '-';
			m = -m;

		}

		char d[8];

		for (int i{ 6 }; i >= 0; --i) {

			d[i] = char('0' + m % 10);
			m /= 10;

		}

		int n{ 7 };

		while (n > 1 && d[n - 1] == '0')
			--n;

		return layout_digits(d, n, e, out);
	}

	// osu!lazers shortest digit round-trip
	inline char* to_string_shortest(float v, char* out) {

		if (v == 0.f) {

			if (std::signbit(v))
				*out++ = '-';

			*out++ = '0';

			return out;
		}

		char b[32];

		const char* end{ std::to_chars(b, b + sizeof b, v, std::chars_format::scientific).ptr };
		const char* q{ b };

		if (*q == '-') {

			*out++ = '-';
			++q;

		}

		char d[16];
		int n{};

		for (; q < end && *q != 'e'; ++q)
			if (*q != '.')
				d[n++] = *q;

		while (n > 1 && d[n - 1] == '0')
			--n;

		int e{};

		std::from_chars(q + 1 + (q[1] == '+'), end, e);

		return layout_digits(d, n, e, out);
	}

	inline char* to_string(float v, char* out, bool exact_text) {

		if (!std::isfinite(v)) {

			const char* t{ std::isnan(v) ? "NaN" : (v < 0 ? "-Infinity" : "Infinity") };

			while (*t)
				*out++ = *t++;

			return out;
		}

		return exact_text ? to_string_shortest(v, out) : to_string_g7(v, out);
	}

	FORCEINLINE void append_hundredths(std::string& out, const int h) {

		if (h <= 0) {

			out += '0';

			return;
		}

		if (h >= 100) {

			out += '1';

			return;
		}

		out += "0.";
		out += char('0' + h / 10);

		if (const auto rem{ h % 10 }; rem)
			out += char('0' + rem);

	}

	inline bool text_ord(float x, bool exact_text, i64& ord) {

		if (!std::isfinite(x) || (x == 0.f && std::signbit(x)))
			return false;

		const float a{ std::abs(x) };

		i64 o{};

		if (exact_text)
			o = std::bit_cast<u32>(a);
		else if (a != 0.f) {

			const auto [m, e] { digits7(a) };

			o = (i64)(e + 50) * 9000000 + (m - 1000000) + 1;

		}

		ord = std::signbit(x) ? -o : o;

		return true;
	}
	
	inline float from_text_ord(i64 ord, bool exact_text) {

		if (ord < -(1ll << 62) || ord > (1ll << 62))
			return 0.f;

		const bool neg{ ord < 0 };
		const i64 o{ neg ? -ord : ord };

		float v{};

		if (exact_text)
			v = float_from_bits((u32)std::min<i64>(o, 0x7f7fffff));
		else if (o > 0) {

			const i64 q{ o - 1 };
			const i64 e{ q / 9000000 - 56 };
			
			if (e <= 40 && e >= -60)
				v = orz_decimal::to_float(q % 9000000 + 1000000, (int)e);

		}

		return neg ? -v : v;
	}

}

}
