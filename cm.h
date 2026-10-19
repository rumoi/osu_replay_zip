#pragma once

// binary context mixing (lpaq style), integer only so every platform makes the same predictions:
// counters in a hashed table -> a gated linear mixer in the logistic domain -> an interpolating refinement (apm)
// -> a 32-bit binary arithmetic coder. Mixer probabilities are 12 bits, coded probabilities 16 bits (P(1) * 65536).

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace orz {

namespace cm {

	// 4096 / (1 + e^(-d / 256)), d clamped to +-2047
	constexpr int squash_of(int d) {

		constexpr int T[33]{
			1, 2, 3, 6, 10, 16, 27, 45, 73, 120, 194, 310, 488, 747, 1101, 1546,
			2047, 2549, 2994, 3348, 3607, 3785, 3901, 3975, 4022, 4050, 4068, 4079, 4085, 4089, 4092, 4093, 4094
		};

		if (d > 2047)
			return 4095;

		if (d < -2047)
			return 1;

		const int w{ d & 127 };

		d = (d >> 7) + 16;

		return (T[d] * (128 - w) + T[d + 1] * w + 64) >> 7;
	}

	// squash over -2048..2048 (the ends are its clamped values) and its inverse, as tables
	constexpr auto SQUASH{ [] {

		std::array<i16, 4097> t{};

		for (int d{ -2048 }; d <= 2048; ++d)
			t[d + 2048] = (i16)squash_of(d);

		return t;
	}() };

	constexpr auto STRETCH{ [] {

		std::array<i16, 4096> t{};

		int pi{};

		for (int x{ -2047 }; x <= 2047; ++x) {

			const int v{ squash_of(x) };

			for (int j{ pi }; j <= v; ++j)
				t[j] = (i16)x;

			pi = v + 1;

		}

		for (int j{ pi }; j < 4096; ++j)
			t[j] = 2047;

		return t;
	}() };

	FORCEINLINE int squash(int d) {
		return SQUASH[std::clamp(d, -2048, 2048) + 2048];
	}

	FORCEINLINE int stretch(int p) {
		return STRETCH[p];
	}

	FORCEINLINE u64 hash(u64 a, u64 b) {

		u64 h{ (a + 1) * 0x9E3779B97F4A7C15ull ^ b * 0xD6E8FEB86659FD93ull };

		h ^= h >> 29;
		h *= 0xBF58476D1CE4E5B9ull;
		h ^= h >> 32;

		return h;
	}

	constexpr u16 COUNTER_LIMIT{ 255 };

	// P(1) in 16 bits; moves 1 / (n + 1.5) of the way to each bit, 1 / (COUNTER_LIMIT + 1.5) once n reaches it
	struct counter {
		u16 p{ 32768 }, n{};
	};

	constexpr auto RATE{ [] {

		std::array<u32, COUNTER_LIMIT + 1> r{};

		for (u32 n{}; n < r.size(); ++n)
			r[n] = 131072u / (2 * n + 3);

		return r;
	}() };

	FORCEINLINE void update(counter& c, int y) {

		const i64 target{ y ? 65535 : 0 };

		c.p = (u16)(c.p + (((target - c.p) * RATE[c.n]) >> 16));

		if (c.n < COUNTER_LIMIT)
			++c.n;

	}

	// counters in lines: one hashed line per (input, context) holds the counters of that context's decisions,
	// so a symbol costs one cache line per input instead of one per decision. No collision check.
	struct line_table {

		std::vector<counter> t;
		u64 mask{};
		u32 width{};

		// optional full hash per line, for exporting priors
		std::vector<u64> keys;

		void init(int bits, u32 width_, bool record_keys = false) {

			width = width_;
			t.assign(size_t(width) << bits, counter{});
			mask = (u64(1) << bits) - 1;

			if (record_keys)
				keys.assign(size_t(1) << bits, 0);

		}

		FORCEINLINE counter* line(u64 h) {

			const size_t i{ (size_t)(h & mask) };

			if (!keys.empty())
				keys[i] = h;

			return &t[i * width];
		}

	};

	constexpr int MAX_INPUTS{ 8 };

	// a weight starts at 0.3 and moves about 0.01 * error * input per bit, which keeps up within a single replay
	constexpr int MIX_WEIGHT{ 19661 }, MIX_RATE{ 40 };

	// refines a probability given a small context, interpolating 33 buckets over the stretched domain
	struct apm {

		std::vector<u16> t;
		size_t lo{};
		int wt{};

		void init(size_t contexts) {

			t.resize(contexts * 33);

			for (size_t c{}; c < contexts; ++c)
				for (int j{}; j < 33; ++j)
					t[c * 33 + j] = (u16)(squash((j - 16) * 128) * 16);

		}

		// 12-bit in, 16-bit out
		FORCEINLINE int refine(int pr, size_t context) {

			const int s{ stretch(pr) + 2048 };

			lo = context * 33 + (s >> 7);
			wt = s & 127;

			return (t[lo] * (128 - wt) + t[lo + 1] * wt) >> 7;
		}

		FORCEINLINE void update(int y) {

			const int target{ y ? 65535 : 0 };

			t[lo] = (u16)(t[lo] + ((((target - t[lo]) * (128 - wt)) >> 7) * 5 >> 8));
			t[lo + 1] = (u16)(t[lo + 1] + ((((target - t[lo + 1]) * wt) >> 7) * 5 >> 8));

		}

	};

	struct bit_encoder {

		u32 x1{}, x2{ 0xFFFFFFFFu };
		std::vector<u8> out;

		// p = P(1) in 16 bits
		FORCEINLINE int code(int y, int p) {

			const u32 xmid{ x1 + (u32)(((u64)(x2 - x1) * (u32)p) >> 16) };

			y ? (x2 = xmid) : (x1 = xmid + 1);

			while (((x1 ^ x2) & 0xFF000000u) == 0) {

				out.push_back(u8(x2 >> 24));

				x1 <<= 8;
				x2 = (x2 << 8) | 255;

			}

			return y;
		}

		std::vector<u8>& finish() {

			for (int i{}; i < 4; ++i) {

				out.push_back(u8(x1 >> 24));

				x1 <<= 8;

			}

			return out;
		}
	};

	struct bit_decoder {

		const u8* p{};
		const u8* end{};
		u32 x1{}, x2{ 0xFFFFFFFFu }, x{};

		bit_decoder(std::span<const u8> data) : p{ data.data() }, end{ data.data() + data.size() } {

			for (int i{}; i < 4; ++i)
				x = (x << 8) | next();

		}

		FORCEINLINE u8 next() {
			return p < end ? *p++ : 0;
		}

		FORCEINLINE int code(int, int pr) {

			const u32 xmid{ x1 + (u32)(((u64)(x2 - x1) * (u32)pr) >> 16) };
			const int y{ x <= xmid };

			y ? (x2 = xmid) : (x1 = xmid + 1);

			while (((x1 ^ x2) & 0xFF000000u) == 0) {

				x1 <<= 8;
				x2 = (x2 << 8) | 255;
				x = (x << 8) | next();

			}

			return y;
		}
	};

	// no output: runs the models only (prior training). A trainer with observe(table, counter index, bit) also sees
	// every counter update.
	struct bit_trainer {

		FORCEINLINE int code(int y, int) {
			return y;
		}

	};

	// one kind of binary decision: per input a counter from its context's line, mixed, then refined.
	// bind() hashes the contexts once, code() then takes the decision's slot in each bound line.
	struct model {

		line_table table;
		std::vector<int> weights; // mixer weights in 16.16 (inputs are stretched probabilities), a set per selector
		apm refine;
		int inputs{};
		counter* lines[MAX_INPUTS]{};

		void init(int inputs_, int table_bits, u32 width, size_t mix_sets, size_t apm_contexts, bool record_keys = false) {

			inputs = inputs_;

			table.init(table_bits, width, record_keys);
			weights.assign(mix_sets * inputs, MIX_WEIGHT);
			refine.init(apm_contexts);

		}

		// salt gives a second set of lines for the same contexts
		FORCEINLINE void bind(const u64* ctx, u64 salt = 0) {

			for (int i{}; i < inputs; ++i)
				lines[i] = table.line(hash(salt * MAX_INPUTS + (u64)i, ctx[i]));

		}

		// the input count known at compile time, so the loops unroll
		template<int N, typename Coder>
		FORCEINLINE int code_n(Coder& c, int y, u32 slot, size_t set, size_t apm_context) {

			counter* k[N];
			int x[N];
			int* const w{ weights.data() + set * N };

			i64 dot{};

			for (int i{}; i < N; ++i) {

				k[i] = lines[i] + slot;
				x[i] = stretch(k[i]->p >> 4);
				dot += (i64)x[i] * w[i];

			}

			const int p{ squash((int)std::clamp<i64>(dot >> 16, -2047, 2047)) };
			const int pa{ refine.refine(p, apm_context) };

			y = c.code(y, std::clamp((p * 16 + 3 * pa) >> 2, 32, 65504));

			const int err{ ((y << 12) - p) * MIX_RATE };

			for (int i{}; i < N; ++i) {

				if constexpr (requires { c.observe(table, size_t{}, y); })
					c.observe(table, (size_t)(k[i] - table.t.data()), y);

				update(*k[i], y);
				w[i] += (int)(((i64)x[i] * err + 0x8000) >> 16);

			}

			refine.update(y);

			return y;
		}

		template<typename Coder>
		FORCEINLINE int code(Coder& c, int y, u32 slot, size_t set, size_t apm_context) {

			switch (inputs) {
			case 1: return code_n<1>(c, y, slot, set, apm_context);
			case 2: return code_n<2>(c, y, slot, set, apm_context);
			case 3: return code_n<3>(c, y, slot, set, apm_context);
			case 4: return code_n<4>(c, y, slot, set, apm_context);
			case 5: return code_n<5>(c, y, slot, set, apm_context);
			case 6: return code_n<6>(c, y, slot, set, apm_context);
			case 7: return code_n<7>(c, y, slot, set, apm_context);
			default: return code_n<8>(c, y, slot, set, apm_context);
			}

		}

		// a single decision on its own contexts
		template<typename Coder>
		FORCEINLINE int code(Coder& c, int y, const u64* ctx, size_t set, size_t apm_context) {

			bind(ctx);

			return code(c, y, 0u, set, apm_context);
		}

	};

}

}
