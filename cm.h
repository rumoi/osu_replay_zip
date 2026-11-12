#pragma once

// binary context mixing (lpaq style), integer only so every platform makes the same predictions:
// counters in a hashed table -> a gated linear mixer in the logistic domain -> a 32-bit binary arithmetic coder. Mixer probabilities are 12 bits, coded probabilities 16 bits (P(1) * 65536).

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <type_traits>
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
			else
				keys.clear();

		}

		FORCEINLINE counter* line(u64 h) {

			const size_t i{ (size_t)(h & mask) };

			if (!keys.empty())
				keys[i] = h;

			return &t[i * width];
		}

	};

	constexpr int MAX_INPUTS{ 8 };

	// the index of the fallback input: one line for all of a model's contexts, so each slot has a counter that every
	// decision at it updates. A decision in a context not seen before still has that counter to go by.
	constexpr int FALLBACK_INPUT{ MAX_INPUTS - 1 };

	// a weight starts at 0.3 and moves about 0.01 * error * input per bit, which keeps up within a single replay.
	// The fallback's starts at 0, so it counts once it has predicted bits the contexts' counters did not.
	constexpr int MIX_WEIGHT{ 19661 }, FALLBACK_WEIGHT{ 0 }, MIX_RATE{ 40 };

	// the weight the i-th of a model's mixed inputs starts at; the fallback is the last
	constexpr int start_weight(size_t i, size_t mixed) {
		return i + 1 < mixed ? MIX_WEIGHT : FALLBACK_WEIGHT;
	}

	struct bit_encoder {

		u32 x1{}, x2{ 0xFFFFFFFFu };
		std::vector<u8> out;

		void check_end() const {}

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
		size_t past_end{};
		u32 x1{}, x2{ 0xFFFFFFFFu }, x{};

		bit_decoder(std::span<const u8> data) : p{ data.data() }, end{ data.data() + data.size() } {

			for (int i{}; i < 4; ++i)
				x = (x << 8) | next();

		}

		FORCEINLINE u8 next() {

			if (p < end)
				return *p++;

			++past_end;

			return 0;
		}

		void check_end() const {

			if (past_end > 16)
				throw std::runtime_error("cm.h> read past end of stream");

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

	struct balanced_bit_encoder : bit_encoder {
		static constexpr bool balanced_prediction{ true };
	};

	struct balanced_bit_decoder : bit_decoder {
		using bit_decoder::bit_decoder;
		static constexpr bool balanced_prediction{ true };
	};

	template<typename Coder>
	constexpr bool uses_balanced_prediction{ [] {
		if constexpr (requires { Coder::balanced_prediction; }) return Coder::balanced_prediction;
		else return false;
	}() };

	struct fastest_bit_encoder : bit_encoder {
		static constexpr bool fallback_only{ true };
	};

	struct fastest_bit_decoder : bit_decoder {
		using bit_decoder::bit_decoder;
		static constexpr bool fallback_only{ true };
	};

	template<typename Coder>
	constexpr bool uses_fallback_only{ [] {
		if constexpr (requires { Coder::fallback_only; }) return Coder::fallback_only;
		else return false;
	}() };

	// no output: runs the models only (prior training). A trainer with observe(table, counter index, bit) also sees
	// every counter update.
	struct bit_trainer {

		void check_end() const {}

		FORCEINLINE int code(int y, int) {
			return y;
		}

	};

	// prior counter statistics do not use the mixed probability or weights.
	struct counter_trainer : bit_trainer {};

	struct balanced_counter_trainer : counter_trainer {
		static constexpr bool balanced_prediction{ true };
	};

	struct fallback_counter_trainer : counter_trainer {
		static constexpr bool fallback_only{ true };
	};

	// one kind of binary decision: per input a counter from its context's line, and the fallback's, mixed.
	// bind() hashes the contexts once, code() then takes the decision's slot in each bound line.
	struct model {

		line_table table;
		std::vector<int> weights; // mixer weights in 16.16 (inputs are stretched probabilities), a set per selector
		int inputs{}; // contexts, fewer than MAX_INPUTS: the fallback is mixed as one more
		counter* lines[MAX_INPUTS]{};

		// the counters a decision mixes
		int mixed() const {
			return inputs + 1;
		}

		void init(int inputs_, int table_bits, u32 width, size_t mix_sets, bool record_keys = false,
			bool fallback_only = false) {

			inputs = inputs_;

			table.init(fallback_only ? 1 : table_bits, width, record_keys);

			if (fallback_only) {

				weights.clear();
				return;

			}
			weights.resize(mix_sets * mixed());

			for (size_t i{}; i < weights.size(); ++i)
				weights[i] = start_weight(i % mixed(), mixed());

		}

		// salt gives a second set of lines for the same contexts
		template<typename Coder = bit_encoder>
		FORCEINLINE void bind(const u64* ctx, u64 salt = 0) {

			if constexpr (uses_fallback_only<Coder>)
				bind_fallback(salt);
			else if constexpr (uses_balanced_prediction<Coder>) {

				lines[0] = table.line(hash(salt * MAX_INPUTS, ctx[0]));
				lines[1] = table.line(hash(salt * MAX_INPUTS + FALLBACK_INPUT, 0));

			} else {

				for (int i{}; i < inputs; ++i)
					lines[i] = table.line(hash(salt * MAX_INPUTS + (u64)i, ctx[i]));

				lines[inputs] = table.line(hash(salt * MAX_INPUTS + FALLBACK_INPUT, 0));

			}

		}

		// frame coding uses salts 0 and 1; the fallback profile stores their lines directly.
		FORCEINLINE void bind_fallback(u64 salt) {

			if (!table.keys.empty())
				table.keys[(size_t)salt] = hash(salt * MAX_INPUTS + FALLBACK_INPUT, 0);

			lines[0] = table.t.data() + (size_t)salt * table.width;

		}

		template<int N, typename Coder = bit_encoder>
		FORCEINLINE void bind_n(const u64* ctx, u64 salt = 0) {

			static_assert(N > 0 && N < MAX_INPUTS);

			if constexpr (uses_fallback_only<Coder>)
				bind_fallback(salt);
			else if constexpr (uses_balanced_prediction<Coder>) {

				lines[0] = table.line(hash(salt * MAX_INPUTS, ctx[0]));
				lines[1] = table.line(hash(salt * MAX_INPUTS + FALLBACK_INPUT, 0));

			} else {

				for (int i{}; i < N; ++i)
					lines[i] = table.line(hash(salt * MAX_INPUTS + (u64)i, ctx[i]));

				lines[N] = table.line(hash(salt * MAX_INPUTS + FALLBACK_INPUT, 0));

			}

		}

		// the input count known at compile time, so the loops unroll
		template<int N, typename Coder>
		FORCEINLINE int code_n(Coder& c, int y, u32 slot, size_t set) {

			if constexpr (uses_fallback_only<Coder>) {

				counter& k{ lines[0][slot] };

				if constexpr (!std::is_base_of_v<counter_trainer, Coder>)
					y = c.code(y, std::clamp((int)k.p, 32, 65504));

				if constexpr (requires { c.observe(table, size_t{}, y); })
					c.observe(table, (size_t)(&k - table.t.data()), y);

				update(k, y);
				return y;

			}

			if constexpr (uses_balanced_prediction<Coder>) {

				const counter& prediction{ lines[0][slot].n >= 8 ? lines[0][slot] : lines[1][slot] };

				if constexpr (!std::is_base_of_v<counter_trainer, Coder>)
					y = c.code(y, std::clamp((int)prediction.p, 32, 65504));

				// Keep both updates, including when the context and fallback alias the same counter.
				for (int i{}; i < 2; ++i) {

					counter& k{ lines[i][slot] };

					if constexpr (requires { c.observe(table, size_t{}, y); })
						c.observe(table, (size_t)(&k - table.t.data()), y);

					update(k, y);

				}

				return y;
			}

			if constexpr (std::is_base_of_v<counter_trainer, Coder>) {

				for (int i{}; i < N; ++i) {

					counter* const k{ lines[i] + slot };

					if constexpr (requires { c.observe(table, size_t{}, y); })
						c.observe(table, (size_t)(k - table.t.data()), y);

					update(*k, y);

				}

				return y;
			}

			counter* k[N];
			int x[N];
			int* const w{ weights.data() + set * N };

			i64 dot{};

			for (int i{}; i < N; ++i) {

				k[i] = lines[i] + slot;
				x[i] = stretch(k[i]->p >> 4);
				dot += (i64)x[i] * w[i];

			}

			const int mixed{ (int)std::clamp<i64>(dot >> 16, -2047, 2047) };
			const int p{ squash(mixed) };
			y = c.code(y, std::clamp(p * 16, 32, 65504));

			const int err{ ((y << 12) - p) * MIX_RATE };

			for (int i{}; i < N; ++i) {

				if constexpr (requires { c.observe(table, size_t{}, y); })
					c.observe(table, (size_t)(k[i] - table.t.data()), y);

				update(*k[i], y);
				w[i] += (int)(((i64)x[i] * err + 0x8000) >> 16);

			}

			return y;
		}

		template<typename Coder>
		FORCEINLINE int code(Coder& c, int y, u32 slot, size_t set) {

			switch (mixed()) {
			case 2: return code_n<2>(c, y, slot, set);
			case 3: return code_n<3>(c, y, slot, set);
			case 4: return code_n<4>(c, y, slot, set);
			case 5: return code_n<5>(c, y, slot, set);
			case 6: return code_n<6>(c, y, slot, set);
			case 7: return code_n<7>(c, y, slot, set);
			default: return code_n<8>(c, y, slot, set);
			}

		}

		// a single decision on its own contexts
		template<typename Coder>
		FORCEINLINE int code(Coder& c, int y, const u64* ctx, size_t set) {

			bind<Coder>(ctx);

			return code(c, y, 0u, set);
		}

	};

	// most models always mix the same number of contexts. Keep that count in their type so binding and coding
	// do not dispatch on it for every bit; the state and arithmetic are the same as model's.
	template<int N>
	struct fixed_model : model {

		static_assert(N > 0 && N < MAX_INPUTS);

		void init(int table_bits, u32 width, size_t mix_sets, bool record_keys = false,
			bool fallback_only = false) {
			model::init(N, table_bits, width, mix_sets, record_keys, fallback_only);
		}

		template<typename Coder = bit_encoder>
		FORCEINLINE void bind(const u64* ctx, u64 salt = 0) {
			bind_n<N, Coder>(ctx, salt);
		}

		template<typename Coder>
		FORCEINLINE int code(Coder& c, int y, u32 slot, size_t set) {
			return code_n<N + 1>(c, y, slot, set);
		}

		template<typename Coder>
		FORCEINLINE int code(Coder& c, int y, const u64* ctx, size_t set) {

			bind<Coder>(ctx);

			return code(c, y, 0u, set);
		}

	};

}

}
