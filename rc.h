#pragma once

// rc_encoder / rc_decoder: 32-bit carryless range coder (Subbotin), total frequency below RC_BOT.
// freq_model: adaptive frequencies over a small alphabet. value_coder: one freq_model per context, values as an
// exact symbol below 'exact', otherwise a bit-length class followed by the low bits.

#include <algorithm>
#include <bit>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace orz {

namespace rc {

	constexpr u32 RC_TOP{ 1u << 24 }, RC_BOT{ 1u << 16 };

	struct rc_encoder {

		u32 low{}, range{ 0xFFFFFFFFu };
		std::vector<u8> out;

		void encode(u32 cum, u32 freq, u32 total) {

			range /= total;
			low += cum * range;
			range *= freq;

			normalize();

		}

		void normalize() {

			while ((low ^ (low + range)) < RC_TOP || (range < RC_BOT && ((range = (0u - low) & (RC_BOT - 1)), true))) {

				out.push_back(u8(low >> 24));

				low <<= 8;
				range <<= 8;

			}

		}

		// up to 32 raw bits, 8 at a time
		void raw(u64 v, int bits) {

			while (bits > 0) {

				const int n{ bits > 8 ? 8 : bits };

				bits -= n;

				encode(u32(v >> bits) & ((1u << n) - 1), 1, 1u << n);

			}

		}

		std::vector<u8>& finish() {

			for (int i{}; i < 4; ++i) {

				out.push_back(u8(low >> 24));

				low <<= 8;

			}

			return out;
		}
	};

	struct rc_decoder {

		const u8* p{};
		const u8* end{};
		u32 low{}, code{}, range{ 0xFFFFFFFFu };

		rc_decoder(std::span<const u8> data) : p{ data.data() }, end{ data.data() + data.size() } {

			for (int i{}; i < 4; ++i)
				code = (code << 8) | next();

		}

		u8 next() {
			return p < end ? *p++ : 0;
		}

		u32 get_freq(u32 total) {

			range /= total;

			const u32 f{ (code - low) / range };

			return f < total ? f : total - 1;
		}

		void decode(u32 cum, u32 freq) {

			low += cum * range;
			range *= freq;

			while ((low ^ (low + range)) < RC_TOP || (range < RC_BOT && ((range = (0u - low) & (RC_BOT - 1)), true))) {

				code = (code << 8) | next();

				low <<= 8;
				range <<= 8;

			}

		}

		u64 raw(int bits) {

			u64 v{};

			while (bits > 0) {

				const int n{ bits > 8 ? 8 : bits };

				bits -= n;

				const u32 f{ get_freq(1u << n) };

				decode(f, 1);

				v = (v << n) | f;

			}

			return v;
		}
	};

	// starts at 1, adds inc per use, halves when the total gets near RC_BOT (inc ~ an additive prior of 1 / inc)
	struct freq_model {

		static constexpr u32 LIMIT{ RC_BOT - 256 };

		std::vector<u16> f;
		u32 total{}, inc{ 20 };

		freq_model() = default; // empty: not created yet (value_coder::model)
		freq_model(int n, u32 inc_) : f(n, 1), total(n), inc{ inc_ } {}

		void update(int s) {

			f[s] = u16(f[s] + inc);
			total += inc;

			if (total > LIMIT) {

				total = 0;

				for (auto& v : f) {

					v = u16((v + 1) / 2);
					total += v;

				}

			}

		}

		void encode(rc_encoder& e, int s) {

			u32 cum{};

			for (int i{}; i < s; ++i)
				cum += f[i];

			e.encode(cum, f[s], total);

			update(s);

		}

		int decode(rc_decoder& d) {

			const u32 target{ d.get_freq(total) };

			u32 cum{};
			int s{};

			while (cum + f[s] <= target)
				cum += f[s++];

			d.decode(cum, f[s]);

			update(s);

			return s;
		}
	};

	struct prior_table {
		static constexpr u16 NONE{ 0xffff };
		const u16* row{}; // [contexts]
		const u32* start{}; // [rows + 1]
		const u16* values{};
		size_t contexts{};
	};

	// exact symbols 0..exact-1, then one class per bit length above that
	struct value_coder {

		int exact;
		int alphabet;
		u32 inc;

		// tables by context: a vector for the small contexts every codec uses, a map for anything larger
		static constexpr u64 DENSE_CONTEXTS{ 1 << 15 };
		std::vector<freq_model> dense;
		std::unordered_map<u64, freq_model> models;

		// optional prior: a context's table starts as if prior_weight symbols had been seen (rows sum to PRIOR_SCALE)
		static constexpr u32 PRIOR_SCALE{ 4096 };
		prior_table prior{};
		u32 prior_weight{};

		explicit value_coder(int exact_ = 16, u32 inc_ = 20) : exact{ exact_ }, inc{ inc_ } {

			// class c covers bit lengths from bit_width(exact) up to 64
			alphabet = exact + (65 - (int)std::bit_width((u64)exact));

		}

		// a new table: flat, or the prior's row for this context
		void create(freq_model& m, u64 ctx) const {

			m = freq_model{ alphabet, inc };

			if (prior.row && ctx < prior.contexts && prior.row[ctx] != prior_table::NONE) {

				const u32 r{ prior.row[ctx] };
				const u16* row{ prior.values + prior.start[r] };
				const int n{ (int)std::min<u32>(prior.start[r + 1] - prior.start[r], (u32)alphabet) }; // values past n are 0

				m.total = 0;

				for (int s{}; s < alphabet; ++s) {

					m.f[s] = u16(1 + (u64(inc) * prior_weight * (s < n ? row[s] : 0u) + PRIOR_SCALE / 2) / PRIOR_SCALE);
					m.total += m.f[s];

				}

			}

		}

		freq_model& model(u64 ctx) {

			if (ctx < DENSE_CONTEXTS) {

				if (ctx >= dense.size())
					dense.resize(size_t(ctx) + 1);

				auto& m{ dense[size_t(ctx)] };

				if (m.f.empty())
					create(m, ctx);

				return m;
			}

			auto it{ models.find(ctx) };

			if (it == models.end()) {

				it = models.emplace(ctx, freq_model{}).first;

				create(it->second, ctx);

			}

			return it->second;
		}

		// optional: the first bit below a class value's leading one coded adaptively per (bit_ctx, class)
		// encode / decode take bit_ctx < top_bit_contexts to use it, or -1 for plain raw bits
		int top_bit_contexts{};
		std::vector<u16> top_bit; // probability of a 1, in 1/4096, per bit_ctx * alphabet + class symbol

		void enable_top_bit(int contexts) {

			top_bit_contexts = contexts;
			top_bit.assign(size_t(contexts) * alphabet, 2048);

		}

		static constexpr int TOP_BIT_SHIFT{ 5 }; // p moves 1/32 of the way per bit

		void encode(rc_encoder& e, u64 ctx, u64 v, int bit_ctx = -1) {

			auto& m{ model(ctx) };

			if (v < (u64)exact) {

				m.encode(e, (int)v);

				return;
			}

			const int len{ (int)std::bit_width(v) }; // >= bit_width(exact)
			const int base{ (int)std::bit_width((u64)exact) };
			const int s{ exact + (len - base) };

			m.encode(e, s);

			int raw_bits{ len - 1 }; // top bit implied

			if (bit_ctx >= 0 && bit_ctx < top_bit_contexts && raw_bits >= 1) {

				u16& p{ top_bit[size_t(bit_ctx) * alphabet + s] };

				const bool bit{ ((v >> (raw_bits - 1)) & 1) != 0 };

				if (bit)
					e.encode(4096 - p, p, 4096);
				else
					e.encode(0, 4096 - p, 4096);

				update_bit(p, bit);

				--raw_bits;

			}

			e.raw(v & ((u64(1) << raw_bits) - 1), raw_bits);

		}

		u64 decode(rc_decoder& d, u64 ctx, int bit_ctx = -1) {

			auto& m{ model(ctx) };

			const int s{ m.decode(d) };

			if (s < exact)
				return (u64)s;

			const int len{ s - exact + (int)std::bit_width((u64)exact) };

			int raw_bits{ len - 1 };
			u64 v{ u64(1) << raw_bits };

			if (bit_ctx >= 0 && bit_ctx < top_bit_contexts && raw_bits >= 1) {

				u16& p{ top_bit[size_t(bit_ctx) * alphabet + s] };

				const bool bit{ d.get_freq(4096) >= u32(4096 - p) };

				if (bit)
					d.decode(4096 - p, p);
				else
					d.decode(0, 4096 - p);

				update_bit(p, bit);

				--raw_bits;

				v |= u64(bit) << raw_bits;

			}

			return v | d.raw(raw_bits);
		}

		static void update_bit(u16& p, bool bit) {

			p = u16(bit ? p + ((4096 - p) >> TOP_BIT_SHIFT) : p - (p >> TOP_BIT_SHIFT));
			p = std::clamp<u16>(p, 16, 4080); // neither side's frequency may reach 0

		}
	};

	// true if value_coder(exact, inc) with this prior weight can never overflow its 16 bit frequencies (a wrapped
	// frequency makes data the decoder can not read back); checked with static_assert next to each coder
	constexpr bool safe_increment(int exact, u32 inc, u32 prior_weight) {

		const u64 alphabet{ u64(exact + (65 - (int)std::bit_width((u64)exact))) };
		const u64 start{ u64(inc) * prior_weight };

		return freq_model::LIMIT - (alphabet - 1) + inc <= 0xffff && 1 + start <= 0xffff && 2 * alphabet + start <= freq_model::LIMIT;
	}

	inline u64 zz(i64 v) {
		return v >= 0 ? u64(v) << 1 : ((u64(-(v + 1))) << 1) | 1;
	}

	inline i64 unzz(u64 v) {
		return (v & 1) ? -(i64)((v + 1) / 2) : (i64)(v / 2);
	}

	// bucket of a magnitude for contexts: exact below 8, then two buckets per power of two
	inline int bk(i64 v) {

		const u64 m{ v < 0 ? 0 - u64(v) : u64(v) };

		if (m < 8)
			return (int)m;

		return std::min(31, 8 + 2 * (63 - std::countl_zero(m - 7)));
	}

}

}
