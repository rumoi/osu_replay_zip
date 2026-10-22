#pragma once

// osu!standard frames as one interleaved stream. Per frame: keys, time delta, position repeat flag and, for a new
// position, both cursor moves (residuals of the move predictor) plus any escape flag and mouse slack; the escape values
// follow the frames. Every value is a sequence of binary decisions coded with context mixing (cm.h), and one templated
// loop serves encoder, decoder and prior training.

#include <algorithm>
#include <array>
#include <bit>
#include <span>
#include <vector>

#include "device_space.h"
#include "orz_time.h"
#include "cm.h"
#include "frames_prior.h"
#include "orz_autopilot.h"

namespace orz {

namespace frames {

	// a delta is coded as its place in the replay's own table (most used first), the tree covers this many entries
	constexpr u32 LISTED{ 64 };

	// the table place as a walk down a fixed binary tree, shaped by how often each place is used (corpus): an entry below
	// 64 is a node, 64 + place a leaf, leaf LISTED = past the listed entries
	constexpr u8 TIME_TREE[64][2]{
		{ 1, 2 }, { 65, 3 }, { 64, 4 }, { 66, 5 }, { 6, 7 }, { 8, 9 }, { 10, 67 }, { 11, 12 },
		{ 72, 71 }, { 13, 70 }, { 14, 15 }, { 69, 16 }, { 17, 68 }, { 18, 80 }, { 79, 78 }, { 77, 76 },
		{ 75, 74 }, { 19, 73 }, { 20, 82 }, { 21, 81 }, { 22, 83 }, { 23, 24 }, { 87, 86 }, { 85, 25 },
		{ 84, 26 }, { 27, 89 }, { 88, 28 }, { 29, 91 }, { 30, 90 }, { 31, 32 }, { 92, 33 }, { 34, 35 },
		{ 36, 37 }, { 93, 38 }, { 39, 40 }, { 41, 42 }, { 43, 44 }, { 45, 128 }, { 46, 94 }, { 47, 48 },
		{ 49, 50 }, { 51, 52 }, { 53, 54 }, { 55, 56 }, { 57, 58 }, { 59, 60 }, { 61, 62 }, { 97, 98 },
		{ 99, 100 }, { 101, 102 }, { 103, 104 }, { 105, 106 }, { 107, 108 }, { 109, 110 }, { 111, 112 }, { 113, 114 },
		{ 115, 116 }, { 117, 118 }, { 119, 120 }, { 121, 122 }, { 123, 124 }, { 125, 126 }, { 127, 63 }, { 95, 96 },
	};

	// each tree entry's parent and the branch to it
	constexpr auto TIME_TREE_UP{ [] {

		std::array<std::array<u8, 2>, 64 + LISTED + 1> up{};

		for (u32 n{}; n < 64; ++n)
			for (u32 b{}; b < 2; ++b)
				up[TIME_TREE[n][b]] = { (u8)n, (u8)b };

		return up;
	}() };

	// a frame this long after the last 60 Hz one is the next
	constexpr i64 TICK_MS{ 14 };

	constexpr int KEY_BITS{ 5 }; // M1 M2 K1 K2 smoke (folded like the key stream), higher bits are escaped

	// valid moves are far below 2^40 and K far below 2^50, only damaged files reach these
	constexpr i64 MAX_MOVE{ 1ll << 40 }, MAX_K{ 1ll << 50 };

	// move residual decisions: 0 zero, 1-3 sign, 4-15 length up, 16 length gate, 17-26 length down, 32-47 long lengths
	constexpr u32 MOVE_STEPS{ 48 };

	struct _frame_data {
		std::vector<int> delta;
		std::vector<u32> keys;           // folded, K1 / K2 imply M1 / M2
		std::vector<u8> repeated;
		std::vector<i64> K[2];           // per new position
		std::vector<int> slack[2];       // per axis, in coding order
		std::vector<u32> escaped[2];     // per axis, positions the device model escapes, ascending
		std::vector<int> dt;             // per new position, ms since the previous one
		std::vector<autopilot::event> moves; // autopilot moves, by the new position they are coded at
	};

	struct _escape {
		float x, pred;
		float near[2]{ NAN, NAN };
	};

	struct _side {
		const dspace::axis_model* axis{};
		std::vector<int> table;          // the replay's delta table
		frame_time::_time_grid grid{};
		e_mode mode{};
		autopilot::_params ap{};
		bool std_default_time{};
	};

	// tables stop growing at this many frames - replays gain little for more (+0.001%)
	constexpr size_t TABLE_FRAMES{ 1 << 14 };

	struct _models {

		cm::model keys_changed, keys_bits, keys_high, time, repeat, move, move_bits, slack, escape, escape_value, ap_event, ap_value;

		// in frames_prior.h order
		std::array<cm::model*, FRAMES_PRIOR_MODELS> all() {
			return { &keys_changed, &keys_bits, &keys_high, &time, &repeat, &move, &move_bits, &slack, &escape, &escape_value, &ap_event, &ap_value };
		}

		// tables follow the replay's length up to TABLE_FRAMES (the frame count of a file to decode is untrusted), and are
		// larger when training priors. The keys and time models, which every mode uses:
		// time_history: the time model also takes the older deltas and the mode's flags (relax, osu!taiko)
		void init_keys_time(size_t frames, bool time_history, bool record_keys = false) {

			const int b{ (int)std::bit_width(std::min(frames, TABLE_FRAMES)) }, extra{ record_keys ? 4 : 0 };

			// single decision models: a counter per line; time / move / slack: a line of counters per context
			keys_changed.init(5, std::clamp(b + 4, 16, 22 + extra), 1, 64, 64, record_keys);
			keys_bits.init(4, std::clamp(b + 2, 14, 20 + extra), 1, 64, 64, record_keys);
			keys_high.init(1, 8, 1, 1, 1, record_keys);
			time.init(time_history ? 6 : 4, std::clamp(b, 12, 16 + extra), 64, 64 * 3 * 3, 64 * 3, record_keys);

		}

		void init(size_t frames, e_mode mode, bool record_keys = false) {

			const int b{ (int)std::bit_width(std::min(frames, TABLE_FRAMES)) }, extra{ record_keys ? 4 : 0 };

			init_keys_time(frames, is_relax(mode), record_keys);

			repeat.init(4, std::clamp(b + 4, 16, 22 + extra), 1, 64, 64, record_keys);
			move.init(6, std::clamp(b + 3, 12, 17 + extra), 32, MOVE_STEPS * 4 * 2, MOVE_STEPS * 32, record_keys);
			move_bits.init(2, std::clamp(b + 2, 12, 20 + extra), 1, 8 * 4, 8 * 32, record_keys);
			slack.init(1, std::clamp(b - 2, 10, 16), 16, 64, 64, record_keys);
			escape.init(5, std::clamp(b + 2, 12, 20 + extra), 1, 16 * 3, 16 * 3, record_keys);
			escape_value.init(2, 10 + extra, 64, 1, 1, record_keys);
			ap_event.init(5, std::clamp(b, 10, 16 + extra), 1, 8, 8, record_keys);
			ap_value.init(2, 12 + extra, 64, 1, 1, record_keys);

		}

	};

	FORCEINLINE int bit_length(i64 v) {

		const u64 m{ v < 0 ? 0 - u64(v) : u64(v) };

		return m == 0 ? 0 : (int)std::bit_width(m);
	}

	FORCEINLINE u64 log_bucket(i64 v) {
		return v <= 0 ? 0 : (u64)std::min(24, (int)std::bit_width((u64)v));
	}

	// a / b in eighths up to 4, 33 when there is no reference
	FORCEINLINE u64 ratio_bucket(i64 a, i64 b) {

		if (b <= 0)
			return 33;

		return (u64)std::clamp<i64>((a * 8 + b / 2) / b, 0, 32);
	}

	// raw bits (escapes), up to 64
	template<typename Coder>
	FORCEINLINE u64 raw(Coder& c, u64 v, int bits) {

		u64 out{};

		for (int b{ bits - 1 }; b >= 0; --b)
			out = (out << 1) | (u64)c.code((int)((v >> b) & 1), 32768);

		return out;
	}

	// Elias gamma of v >= 1 with raw bits
	template<typename Coder>
	FORCEINLINE u64 gamma(Coder& c, u64 v) {

		const int len{ v ? 63 - std::countl_zero(v) : 0 };

		int n{};

		while (n < 63 && c.code(len > n, 32768))
			++n;

		return (u64(1) << n) | raw(c, v, n);
	}

	FORCEINLINE u64 delta_bucket(int d) {
		return (u64)std::clamp(d, -2, 40) + 2;
	}

	// keys as press / release events: whether any key changes, then each key's flip, against the rhythm of earlier
	// presses and holds. Flips from bit `bits` up are coded raw.
	struct _key_coder {

		static constexpr int MAX_BITS{ 31 };

		int bits;

		i64 last_change{ -100000 }, last_press{ -100000 }, press_interval{}, prev_hold{};
		i64 press_start[MAX_BITS]{}, key_last_press[MAX_BITS]{}, key_interval[MAX_BITS]{}, key_prev_hold[MAX_BITS]{};
		int frames_since_change{}, last_key{ -1 }, prev_key{ -1 };

		explicit _key_coder(int bits_) : bits{ std::clamp(bits_, 1, MAX_BITS) } {

			for (auto& v : key_last_press)
				v = -100000;

		}

		// the keys of the frame starting at now, after pk
		template<typename Coder>
		u32 code(Coder& c, _models& M, u32 pk, u32 want, i64 now) {

			const bool down{ (pk & ((1u << bits) - 1)) != 0 };

			i64 held{};

			if (down) {

				i64 earliest{ now };

				for (int b{}; b < bits; ++b)
					if ((pk >> b) & 1)
						earliest = std::min(earliest, press_start[b]);

				held = now - earliest;

			}

			const u64 rhythm{ ratio_bucket(now - last_press, press_interval) }, hold{ down ? ratio_bucket(held, prev_hold) : 34 };

			const u64 s{ std::min<u64>(pk, 63) };
			const u64 ctx[5]{
				s * 32 + log_bucket(now - last_change),
				s * 40 + rhythm,
				s * 40 + hold,
				(s * 32 + log_bucket(press_interval)) * 32 + log_bucket(now - last_press),
				(s * 32 + (u64)std::min(frames_since_change, 31)) * 32 + log_bucket(now - last_change),
			};

			if (!M.keys_changed.code(c, want != pk, ctx, s, s))
				return pk;

			const u32 flip{ want ^ pk };

			u32 flipped{};

			for (int b{}; b < bits; ++b) {

				const u64 is_down{ (pk >> b) & 1 };
				const u64 key_rhythm{ ratio_bucket(now - key_last_press[b], key_interval[b]) };
				const u64 key_hold{ is_down ? ratio_bucket(now - press_start[b], key_prev_hold[b]) : 34 };
				const u64 bctx[4]{
					(u64)b * 64 + log_bucket(now - key_last_press[b]) * 2 + is_down,
					(s * 64 + (flipped & 63)) * 32 + (u64)b,
					(((u64)b * 33 + (u64)(last_key + 1)) * 33 + (u64)(prev_key + 1)) * 2 + is_down,
					((u64)b * 40 + hold) * 2 + is_down,
				};

				const size_t set{ (size_t)b * 2 + (size_t)is_down };

				flipped |= (u32)M.keys_bits.code(c, (int)((flip >> b) & 1), bctx, set, set) << b;

			}

			// a change above the modelled keys (never seen in osu!standard, kept for exactness)
			const u64 hctx[1]{ flipped != 0 };
			const u32 high{ flipped == 0 || M.keys_high.code(c, (flip >> bits) != 0, hctx, 0, 0) ?
				(u32)raw(c, flip >> bits, 32 - bits) : 0u };

			return pk ^ (flipped | (high << bits));
		}

		// the frame ending at t went from pk to k
		void update(u32 pk, u32 k, i64 t) {

			if (k == pk) {

				++frames_since_change;

				return;
			}

			const u32 pressed{ k & ~pk }, released{ pk & ~k };

			for (int b{}; b < bits; ++b) {

				if ((released >> b) & 1) {

					prev_hold = t - press_start[b];
					key_prev_hold[b] = prev_hold;

				}

				if ((pressed >> b) & 1) {

					key_interval[b] = t - key_last_press[b];
					key_last_press[b] = t;

				}

			}

			if (pressed & ((1u << bits) - 1)) {

				press_interval = t - last_press;
				last_press = t;

				for (int b{}; b < bits; ++b) {

					if ((pressed >> b) & 1) {

						press_start[b] = t;
						prev_key = last_key;
						last_key = b;

					}

				}

			}

			last_change = t;
			frames_since_change = 0;

		}

	};

	// a frame time as its place in the replay's own delta table: the first LISTED places down TIME_TREE, a counter per
	// tree node; later ones (rare) as an escaped index
	struct _time_coder {

		const std::vector<int>& table;
		frame_time::_time_grid grid;
		int zero_run{}, prev_zero_run{}, one_run{};
		i64 now{}, last_tick{}; // the time so far, the last 60 Hz frame's (the game records one at 60 Hz and others between)

		// the delta of frame f = past.size(). kc / kc1: keys changed in this / the previous frame, repeats: the repeat flags of
		// the last three frames (below 27), only relax's time model takes them
		template<typename Coder>
		int code(Coder& c, cm::model& m, std::span<const int> past, int want, u64 phase, u64 kc, u64 kc1, u64 repeats) {

			const size_t f{ past.size() };
			const u64 d1{ f >= 1 ? delta_bucket(past[f - 1]) : 50 }, d2{ f >= 2 ? delta_bucket(past[f - 2]) : 50 };

			u64 ctx[6]{
				(d1 * 64 + d2) * 3 + kc,
				(((u64)std::min(zero_run, 15) * 16 + (u64)std::min(prev_zero_run, 15)) * 16 + (u64)std::min(one_run, 15)) * 3 + kc,
				((phase * 64 + d1) * 3 + kc) * 2 + kc1,
				((u64)grid.bin(last_tick) * 32 + (u64)std::clamp<i64>(now - last_tick, 0, 31)) * 2 + kc,
			};

			// relax
			if (m.inputs > 4) {

				const u64 d3{ f >= 3 ? delta_bucket(past[f - 3]) : 50 }, d4{ f >= 4 ? delta_bucket(past[f - 4]) : 50 };

				ctx[4] = ((d1 * 64 + d2) * 64 + d3) * 64 + d4;
				ctx[5] = d1 * 27 + repeats;

			}

			// mixer weights and apm also by whether the keys change on this frame or did on the last
			const size_t regime{ d1 <= 5 ? 0u : d1 <= 13 ? 1u : 2u }, key_change{ kc ? 2u : kc1 ? 1u : 0u };

			m.bind(ctx);

			const u32 n{ (u32)table.size() }, listed{ std::min(n, LISTED) };
			const u32 index{ (u32)(std::find(table.begin(), table.end(), want) - table.begin()) };

			// the branches to the leaf, from the leaf up
			u8 branch[64];
			int depth{};

			for (u32 e{ 64 + (index < listed ? index : LISTED) }; e != 0; e = TIME_TREE_UP[e][0])
				branch[depth++] = TIME_TREE_UP[e][1];

			u32 node{};

			while (node < 64) {

				const size_t step{ (size_t)node * 3 };
				const int y{ depth ? branch[--depth] : 0 };

				node = TIME_TREE[node][m.code(c, y, node, (step + regime) * 3 + key_change, step + key_change)];

			}

			// a leaf past the table (a damaged file) reads as the escape
			u32 at{ std::min(node - 64, listed) };

			if (at == listed && listed < n)
				at = listed + (u32)(gamma(c, index - listed + 1) - 1);

			const int d{ n ? table[std::min(at, n - 1)] : 0 };

			if (d == 0)
				++zero_run;
			else {

				prev_zero_run = zero_run;
				zero_run = 0;

			}

			one_run = d == 1 ? one_run + 1 : 0;
			now += d;

			if (now - last_tick >= TICK_MS)
				last_tick = now;

			return d;
		}

	};

	// 0, then sign and bit length (up to 15) as 1-32
	FORCEINLINE int signed_length(i64 v) {
		return v == 0 ? 0 : (v < 0 ? 1 : 2) + 2 * std::min(bit_length(v), 15);
	}

	struct _move_features {
		int mode, order, bkp, sign, scale, regime, first_res, prev_res, dt, since_move, gap;
		int accel, vel_gap; // signed_length of the last change of this axis's move, and of the last move scaled to dt less the prediction
		int prev2; // signed_length of the residual before prev_res
	};

	// what the move features remember per axis
	struct _axis_history {

		i64 scale16{ 64 }, prev_res{}, prev_res2{}; // scale16: running mean of |residual| * 16
		int since_move{}, gap{};

		FORCEINLINE void fill(_move_features& f, int dt) const {

			f.scale = std::min(31, bit_length(scale16 >> 2));
			f.regime = dt <= 3 ? 0 : dt <= 11 ? 1 : 2;
			f.prev_res = (int)std::clamp<i64>(prev_res, -20, 20);
			f.prev2 = signed_length(prev_res2);
			f.dt = dt;
			f.since_move = std::min(since_move, 15);
			f.gap = std::min(gap, 15);

		}

		FORCEINLINE void push(i64 res, i64 d) {

			scale16 += ((std::min<i64>(res < 0 ? -res : res, 1 << 24) << 4) - scale16) >> 2;
			prev_res2 = prev_res;
			prev_res = res;

			if (d != 0) {

				gap = since_move;
				since_move = 0;

			} else
				++since_move;

		}

	};

	// move residual: zero, sign, bit length (unary from about the usual length), two modelled bits below the leading one,
	// then raw bits
	template<typename Coder>
	i64 move_residual(Coder& c, _models& M, i64 r, const _move_features& f) {

		const u64 ctx[6]{
			(((u64)f.bkp * 3 + f.regime) * 2 + f.order) * 5 + f.mode,
			((((u64)f.accel * 33 + f.vel_gap) * 2 + f.order) * 3 + f.regime) * 32 + f.bkp,
			(((u64)f.scale * 20 + std::min(f.first_res, 19)) * 2 + f.order) * 3 + f.regime,
			((u64)std::min(f.dt, 40) * 32 + f.bkp) * 2 + f.order,
			(((u64)signed_length(f.prev_res) * 33 + f.prev2) * 3 + f.regime) * 2 + f.order,
			((u64)f.since_move * 16 + f.gap) * 41 + (u64)(f.prev_res + 20),
		};

		const size_t apc{ (size_t)std::min(f.scale, 31) };

		// steps as MOVE_STEPS, the long lengths in a second line
		M.move.bind(ctx);

		const auto decide = [&](int y, u32 step) {

			const u32 slot{ step < 32 ? step : step & 15 };

			return M.move.code(c, y, slot, ((size_t)step * 4 + (size_t)f.regime) * 2 + (size_t)f.order, (size_t)step * 32 + apc);
		};

		if (decide(r == 0, 0))
			return 0;

		const int negative{ decide(r < 0, 1 + (u32)f.sign) };

		const u64 v{ r < 0 ? 0 - u64(r) : u64(r) };
		const int len{ v ? 63 - std::countl_zero(v) : 0 };

		// the length starts from about the bit length of the running mean residual: up as plain unary, down on its own steps
		const int start{ std::clamp(f.scale - 4, 0, 11) };

		int n{};

		if (start == 0 || decide(len >= start, 16)) {

			n = start;

			while (n < 12 && decide(len > n, 4 + (u32)n))
				++n;

		} else {

			n = start - 1;

			while (n > 0 && !decide(len >= n, 16 + (u32)n))
				--n;

		}

		if (n == 12) {

			M.move.bind(ctx, 1);

			while (n < 62 && decide(len > n, 32 + (u32)std::min(n - 12, 15)))
				++n;

		}

		const u64 scale_ctx{ (u64)f.scale * 4 + f.regime };

		u64 out{ 1 };

		for (int b{ n - 1 }, k{}; b >= 0; --b, ++k) {

			const int bit{ (int)((v >> b) & 1) };

			if (k < 2) {

				const u64 id{ (u64)k * 4 + (out & ((u64(1) << k) - 1)) };
				const u64 cx[2]{ ((ctx[0] * 64 + (u64)n) * 64 + id) * 2, ((scale_ctx * 64 + (u64)n) * 64 + id) * 2 + 1 };

				out = (out << 1) | (u64)M.move_bits.code(c, bit, cx, (size_t)id * 4 + (size_t)f.regime, (size_t)id * 32 + (size_t)std::min(n, 31));

			} else
				out = (out << 1) | (u64)c.code(bit, 32768);

		}

		return negative ? -(i64)out : (i64)out;
	}

	// mouse slack: the candidate index (<= 2 * MOUSE_SLACK_RANGE), unary
	template<typename Coder>
	int slack_value(Coder& c, cm::model& m, int k, int axis, i64 d) {

		const u64 ctx[1]{ (u64)axis * 64 + (u64)bit_length(d) };

		m.bind(ctx);

		u64 v{};

		while (v < 2 * (u64)dspace::MOUSE_SLACK_RANGE) {

			const u32 slot{ (u32)std::min<u64>(v, 15) };

			if (!m.code(c, (u64)k > v, slot, slot, slot))
				break;

			++v;

		}

		return (int)v;
	}

	// an autopilot move's field: zero, sign, bit length (unary), then the bits under the leading one, the first two modelled.
	// Valid fields are far below MAX_MOVE.
	template<typename Coder>
	i64 ap_number(Coder& c, cm::model& m, i64 v, u64 field, u64 ctx) {

		const u64 cx[2]{ field * 4096 + ctx, field };

		m.bind(cx);

		if (m.code(c, v == 0, 0u, 0, 0))
			return 0;

		const int negative{ m.code(c, v < 0, 1u, 0, 0) };

		const u64 a{ v < 0 ? 0 - u64(v) : u64(v) };
		const int len{ a ? 63 - std::countl_zero(a) : 0 };

		int n{};

		while (n < 62 && m.code(c, len > n, 2u + (u32)std::min(n, 29), 0, 0))
			++n;

		u64 out{ 1 };

		for (int b{ n - 1 }, k{}; b >= 0; --b, ++k) {

			const int bit{ (int)((a >> b) & 1) };

			out = (out << 1) | (u64)(k < 2 ? m.code(c, bit, 32u + (u32)std::min(n, 15) * 2 + (u32)k, 0, 0) : c.code(bit, 32768));

		}

		return std::clamp(negative ? -(i64)out : (i64)out, -MAX_MOVE, MAX_MOVE);
	}

	// an escaped position's value against decode_axis's prediction: the distance in 7 digit mantissas (in ulps for
	// shortest round-trip text) when near, else the raw float. prev_len: per axis, the last distance's bit length.
	template<typename Coder>
	float escape_value(Coder& c, cm::model& m, const _escape& e, int a, bool exact_text, int& prev_len, bool encoding) {

		const float x{ e.x }, pred{ e.pred };

		i64 diff{};

		const bool near{ encoding && dspace::escape_diff(x, pred, exact_text, diff) };
		const u64 kind{ (u64)a * 2 + exact_text };
		const u64 ctx[2]{ kind, kind * 64 + (u64)prev_len };

		m.bind(ctx);

		if (!std::isnan(e.near[0])) {

			const int k{ encoding ? (x == e.near[0] ? 1 : x == e.near[1] ? 2 : 0) : 0 };

			if (m.code(c, k != 0, 40u, 0, 0)) {

				prev_len = 34;

				return e.near[m.code(c, k == 2, 41u, 0, 0)];
			}

		}

		if (!std::isfinite(pred) || !m.code(c, near, 0u, 0, 0)) {

			prev_len = 63;

			return float_from_bits((u32)raw(c, std::bit_cast<u32>(x), 32));
		}

		const u64 v{ rc::zz(diff) + 1 };
		const int len{ 63 - std::countl_zero(v) };

		int n{};

		while (n < 33 && m.code(c, len > n, 1 + (u32)n, 0, 0))
			++n;

		prev_len = n;

		const i64 d{ std::clamp<i64>(rc::unzz(((u64(1) << n) | raw(c, v, n)) - 1), -(1ll << 31), 1ll << 31) };

		return dspace::escape_from_diff(pred, d, exact_text);
	}

	// autopilot, off the knots: the line through the last two new positions, in 7 digit ordinals against prev_K
	FORCEINLINE i64 ap_extrapolate(const autopilot::trail& seen, i64 now, int a, i64 prev_K) {

		i64 o;

		return seen.n >= 2 && dotnet::text_ord(seen.line(now, a), false, o) ? std::clamp(o - prev_K, -MAX_MOVE, MAX_MOVE) : 0;
	}

	// autopilot in the frame loop: the knots so far, whether the last new position was on them, the last new positions at
	// different times (between knots the speed is constant), moves so far, the last one's kind and time
	struct _autopilot {
		autopilot::path path;
		autopilot::trail seen{};
		bool covered{};
		int since{};
		size_t at{};
		u64 last{ 2 };
		i64 last_time{ 1 };
	};

	// autopilot: whether a move starts at new position i (past the knots), and the move
	template<typename Coder>
	void ap_move(Coder& c, _models& M, const _frame_data& fd, const _side& side, _autopilot& st, i64 now, size_t i, bool encoding) {

		autopilot::path& ap{ st.path };
		const u64 last{ st.last };

		const bool is_knot{ st.covered && !ap.k.empty() };
		const i64 gap{ ap.k.empty() ? 1 << 20 : now - ap.k.back().t };
		const u64 sb{ log_bucket(st.since) }, gb{ log_bucket(gap) };
		const u64 ctx[5]{ sb * 4 + last, gb * 4 + last, (sb * 32 + gb) * 2 + is_knot, (u64)is_knot * 4 + last, sb * 2 + is_knot };

		const bool want{ encoding && st.at < fd.moves.size() && fd.moves[st.at].sample == i };

		if (!M.ap_event.code(c, want, ctx, (size_t)last * 2 + is_knot, (size_t)last * 2 + is_knot))
			return;

		autopilot::event e{ want ? fd.moves[st.at++] : autopilot::event{} };

		auto& m{ M.ap_value };

		e.type = (autopilot::kind)std::clamp<i64>(ap_number(c, m, (i64)e.type, 0, last), 0, 1);
		e.wait = ap_number(c, m, e.wait, 1, (u64)e.type * 2 + is_knot);

		// a move straight after a line move of the same kind: its time against that one's
		const bool chained{ is_knot && last == 0 && e.type == autopilot::kind::line && e.wait == 0 };

		e.time = chained ? st.last_time + ap_number(c, m, e.time - st.last_time, 8, 0) : ap_number(c, m, e.time - 1, 2, (u64)e.type) + 1;

		for (int a{}; a < 2; ++a) {

			if (!is_knot)
				e.from_ulp[a] = ap_number(c, m, e.from_ulp[a], 3, (u64)a);

			e.to_int[a] = ap_number(c, m, e.to_int[a], 4, (u64)a * 3 + (a ? (u64)e.to_int[0] : 2)) != 0;
			e.to[a] = ap_number(c, m, e.to[a], e.to_int[a] ? 5 : 6, (u64)a * 2 + (u64)e.type);

			if (!e.to_int[a])
				e.to_ulp[a] = ap_number(c, m, e.to_ulp[a], 7, (u64)a);

		}

		// the move starts from the last knot, or from the last new position when the knots ended before it
		float anchor[2];

		for (int a{}; a < 2; ++a)
			anchor[a] = is_knot ? ap.k.back().p[a] : dotnet::from_text_ord(fd.K[a][i - 1], false);

		const i64 anchor_t{ is_knot ? ap.k.back().t : st.seen.p[2].t };

		ap.add(autopilot::resolve(e, anchor, is_knot, anchor_t, is_knot ? ap.tail() : st.seen), anchor_t, is_knot, side.ap.sample_ms());

		st.last = (u64)e.type;
		st.last_time = std::clamp<i64>(e.time, 1, autopilot::MAX_MOVE_MS);
		st.since = 0;
	}

	// encoding reads fd, decoding (encoding false) fills it; side must match on both ends
	template<typename Coder>
	void frame_loop(Coder& c, _models& M, _frame_data& fd, const _side& side, size_t frames, bool encoding) {

		if (!encoding) {

			fd.delta.assign(frames, 0);
			fd.keys.assign(frames, 0);
			fd.repeated.assign(frames, 0);

			for (int a{}; a < 2; ++a) {

				fd.K[a].clear();
				fd.slack[a].clear();
				fd.escaped[a].clear();

			}

			fd.dt.clear();

		}

		i64 now{};

		_key_coder keys{ KEY_BITS };
		_time_coder time{ side.table, side.grid };

		// repeat flag
		int repeat_run{};
		i64 repeat_since{}, last_move{};

		// moves
		dspace::move_predictor mp{};
		_axis_history history[2]{};
		size_t i{}, escape_at[2]{}, slack_at[2]{}, segment_at[2]{};
		i64 elapsed{}, first_res{};

		// autopilot
		const autopilot::_screen ap_screen{ side.ap };
		_autopilot ap{ { {}, 0, 4 * frames + 4096 } };

		// escapes: whether an axis has any, positions since its last, how many so far
		bool escapes[2]{}, first_escaped{};
		int since_escape[2]{ 64, 64 };
		i64 escape_count[2]{};

		for (int a{}; a < 2; ++a)
			escapes[a] = c.code(encoding ? (int)!fd.escaped[a].empty() : 0, 32768) != 0;

		for (size_t f{}; f < frames; ++f) {

			if ((f & 1023) == 0)
				c.check_end();

			const u32 pk{ f ? fd.keys[f - 1] : 0u };
			const u64 phase{ side.grid.bin(now) };

			fd.keys[f] = keys.code(c, M, pk, encoding ? fd.keys[f] : 0u, now);

			const u32 k{ fd.keys[f] };
			const bool changed{ k != pk };

			{ // time

				const u64 kc1{ f ? (u64)(fd.keys[f - 1] != (f > 1 ? fd.keys[f - 2] : 0u)) : 0 };
				// the repeat flag back frames ago, 2 before the first
				const auto repeated_at = [&](size_t back) {

					return f >= back ? (u64)fd.repeated[f - back] : u64{ 2 };
				};

				// the repeat flags of the last three frames, for relax
				const u64 r{ is_relax(side.mode) ? (repeated_at(1) * 3 + repeated_at(2)) * 3 + repeated_at(3) : 0 };

				if (side.std_default_time && f < 2)
					fd.delta[f] = f ? -1 : 0;
				else
					fd.delta[f] = time.code(c, M.time, std::span{ fd.delta }.first(f), encoding ? fd.delta[f] : 0, phase, changed, kc1, r);

			}

			const int d_f{ fd.delta[f] };

			elapsed += d_f;

			bool repeated{};

			if (f) { // repeat flag

				repeat_since += d_f;

				const auto bucket = [](i64 v, i64 hi) { return (u64)std::clamp<i64>(v, -2, hi) + 2; };

				const u64 dd{ bucket(d_f, 30) }, s{ bucket(repeat_since, 40) };
				const u64 p1{ fd.repeated[f - 1] }, p2{ f >= 2 ? (u64)fd.repeated[f - 2] : 2 };
				const u64 moved{ (u64)std::min<i64>(last_move, 40) };

				// bit length of the move the predictor expects by now (21 before the first position)
				const int since{ (int)std::clamp<i64>(elapsed, INT_MIN, INT_MAX) };
				const u64 expect{ i ? (u64)std::min(bit_length(std::abs(mp.pred(0, since)) + std::abs(mp.pred(1, since))), 20) : 21 };

				const u64 ctx[4]{
					((s * 40 + dd) * 4 + (u64)std::min(repeat_run, 3)) * 2 + (u64)changed,
					((dd * 3 + p1) * 3 + p2) * 2 + (u64)changed,
					(s * 41 + moved) * 16 + (u64)std::min(repeat_run, 15),
					(expect * 41 + s) * 40 + dd,
				};

				const size_t set{ (size_t)std::min<u64>(dd, 20) * 3 + (size_t)std::min<u64>(p1, 2) };

				repeated = M.repeat.code(c, encoding ? fd.repeated[f] : 0, ctx, set, set) != 0;

				fd.repeated[f] = repeated;

				if (repeated)
					++repeat_run;
				else {

					repeat_run = 0;
					repeat_since = 0;

				}

			}

			if (!repeated) { // moves, as dspace::move_loop

				const int dt{ (int)std::clamp<i64>(elapsed, INT_MIN, INT_MAX) };

				if (!encoding)
					fd.dt.push_back(dt);

				int pdt{ dt };

				if (i && elapsed - d_f > dspace::move_predictor::REST_MS) {

					mp.rest((int)std::clamp<i64>(elapsed - d_f, INT_MIN, INT_MAX));
					pdt = d_f;

				}

				elapsed = 0;

				const int first{ mp.first(pdt) };

				i64 first_d{}, first_pred{};

				// autopilot: a move when the knots end before this frame, then the knots' position
				i64 ap_pred[2]{};
				bool ap_known{};

				if (side.ap.on) {

					const i64 t_abs{ now + d_f };

					if (i && !ap.path.covers(t_abs))
						ap_move(c, M, fd, side, ap, t_abs, i, encoding);

					ap_known = ap.path.covers(t_abs);
					ap.covered = ap_known;
					++ap.since;

					if (ap_known) {

						float v[2];

						ap.path.predict(t_abs, v);

						for (int a{}; a < 2; ++a)
							ap_known &= dotnet::text_ord(ap_screen.field(v[a], a), false, ap_pred[a]);

					}

				}

				for (int q{}; q < 2; ++q) {

					const int a{ q ? 1 - first : first };

					i64 pr{ mp.pred(a, pdt) };

					if (q)
						pr = dspace::move_predictor::scaled(pr, first_d, first_pred);

					const i64 prev_K{ i ? fd.K[a][i - 1] : 0 };

					if (side.ap.on)
						pr = ap_known ? ap_pred[a] - prev_K : ap_extrapolate(ap.seen, now + d_f, a, prev_K);

					const dspace::axis_params& params{ side.axis[a].at(i, segment_at[a]) };

					_move_features mf{};

					mf.mode = (int)params.device.kind;
					mf.bkp = rc::bk(pr);
					mf.sign = pr < 0 ? 0 : pr == 0 ? 1 : 2;

					if (side.ap.on)
						mf.mode = ap_known ? 5 : 6;

					history[a].fill(mf, dt);

					mf.order = q;
					mf.first_res = q ? bit_length(first_res) * 2 + (first_res < 0) : 0;
					mf.accel = mp.n >= 2 ? signed_length(mp.hist[a][0] - mp.hist[a][1]) : 0;
					mf.vel_gap = mp.n >= 1 ? signed_length(dspace::move_predictor::round_div(mp.hist[a][0] * dspace::move_predictor::ms(pdt),
						dspace::move_predictor::ms(mp.hist_dt[0])) - pr) : 0;

					const i64 res{ std::clamp(move_residual(c, M, encoding ? fd.K[a][i] - prev_K - pr : 0, mf), -MAX_MOVE, MAX_MOVE) };
					const i64 d{ std::clamp(res + pr, -MAX_MOVE, MAX_MOVE) };

					if (!encoding)
						fd.K[a].push_back(std::clamp(prev_K + d, -MAX_K, MAX_K));

					history[a].push(res, d);

					if (!q) {

						first_d = d;
						first_pred = pr;
						first_res = res;

					}

					mp.push(a, d);

					bool escaped{};

					if (escapes[a]) {

						const auto& esc{ fd.escaped[a] };
						const bool want{ encoding && escape_at[a] < esc.size() && esc[escape_at[a]] == i };
						const u64 since{ (u64)std::min(since_escape[a], 63) }, sb{ log_bucket(since_escape[a]) };
						const u64 other{ q ? (u64)first_escaped : 2 };
						const u64 ectx[5]{
							since * 2 + (u64)a,
							((u64)std::min(bit_length(d), 40) * 5 + (u64)params.device.kind) * 2 + (u64)a,
							sb * 32 + (u64)std::min(bit_length(res), 31),
							other * 64 + since,
							log_bucket(escape_count[a] * 4096 / (i64)(i + 1)) * 32 + sb,
						};

						const size_t set{ (size_t)std::min<u64>(sb, 15) * 3 + (size_t)other };

						escaped = M.escape.code(c, want, ectx, set, set) != 0;

						if (escaped) {

							if (encoding)
								++escape_at[a];
							else
								fd.escaped[a].push_back((u32)i);

							++escape_count[a];

						}

						since_escape[a] = escaped ? 0 : since_escape[a] + 1;

					}

					if (!q)
						first_escaped = escaped;

					if (!escaped && params.device.kind == dspace::axis_mode::mouse && d != 0) {

						const int s{ slack_value(c, M.slack, encoding ? fd.slack[a][slack_at[a]] : 0, a, d) };

						if (!encoding)
							fd.slack[a].push_back(s);

						++slack_at[a];

					}

				}

				mp.advance(pdt);

				if (side.ap.on) {

					const i64 t_abs{ now + d_f };

					ap.seen.push({ t_abs, { dotnet::from_text_ord(fd.K[0][i], false), dotnet::from_text_ord(fd.K[1][i], false) } });

				}

				last_move = std::abs(fd.K[0][i] - (i ? fd.K[0][i - 1] : 0)) + std::abs(fd.K[1][i] - (i ? fd.K[1][i - 1] : 0));

				++i;

			}

			now += d_f;

			keys.update(pk, k, now);

		}

	}

	// frames_prior.h holds a prior per set: osu!standard by the device of the replay's model (rational, mouse,
	// absolute), relax by the same, relax and osu!standard without a device model (mostly relax osu!catch and
	// autopilot), then osu!taiko, osu!catch and osu!mania (modes::kind order)
	enum prior_set : u32 {
		set_rational, set_mouse, set_absolute, set_relax_rational, set_relax_mouse, set_relax_absolute, set_relax_other,
		set_no_device, set_taiko
	};

	inline u32 prior_set(const _side& side) {

		// the higher mode of the axes' first segments
		auto device{ dspace::axis_mode::none };

		for (int a{}; a < 2; ++a)
			if (!side.axis[a].seg.empty())
				device = std::max(device, side.axis[a].seg[0].p.device.kind);

		if (device < dspace::axis_mode::rational || device > dspace::axis_mode::absolute)
			return is_relax(side.mode) ? set_relax_other : set_no_device;

		return (is_relax(side.mode) ? set_relax_rational : set_rational) + (u32)device - (u32)dspace::axis_mode::rational;
	}

	// a starting state: counters by context hash (p | confidence << 16 | slot << 24), then per model the mixer weights
	// and apm table, which a model takes only when the sizes match
	struct _prior {
		const u32* start;
		const u32* keys; // the low 32 bits, all a table's index takes
		const u32* values;
		const u32* weight_start;
		const i32* weights;
		const u32* apm_start;
		const u16* apm;
	};

	inline _prior builtin_prior(u32 set) {
		return { FRAMES_PRIOR_START[set], FRAMES_PRIOR_KEYS, FRAMES_PRIOR_VALUES, FRAMES_PRIOR_WEIGHT_START[set],
			FRAMES_PRIOR_WEIGHTS, FRAMES_PRIOR_APM_START[set], FRAMES_PRIOR_APM };
	}

	// models: in frames_prior.h order, nullptr where a mode has no such model
	inline void seed(const std::array<cm::model*, FRAMES_PRIOR_MODELS>& models, const _prior& prior) {

		for (u32 m{}; m < FRAMES_PRIOR_MODELS; ++m) {

			cm::model* md{ models[m] };

			if (!md || md->table.t.empty())
				continue;

			for (u32 j{ prior.start[m] }; j < prior.start[m + 1]; ++j) {

				const u32 value{ prior.values[j] };

				auto& k{ md->table.line(prior.keys[j])[value >> 24] };

				k.p = (u16)(value & 0xFFFF);
				k.n = (u16)((value >> 16) & 0xFF);

			}

			const u32 w0{ prior.weight_start[m] }, w1{ prior.weight_start[m + 1] };

			if (md->weights.size() == w1 - w0)
				std::copy(prior.weights + w0, prior.weights + w1, md->weights.begin());

			const u32 a0{ prior.apm_start[m] }, a1{ prior.apm_start[m + 1] };

			if (md->refine.t.size() == a1 - a0)
				std::copy(prior.apm + a0, prior.apm + a1, md->refine.t.begin());

		}

	}

	inline void seed_priors(_models& M, const _side& side) {
		seed(M.all(), builtin_prior(prior_set(side)));
	}

	// the frames, then each axis's escape values in order
	inline std::vector<u8> encode(_models& M, _frame_data& fd, const _side& side, size_t frames, const std::vector<_escape> (&values)[2],
		bool exact_text) {

		M.init(frames, side.mode);
		seed_priors(M, side);

		cm::bit_encoder e{};

		frame_loop(e, M, fd, side, frames, true);

		for (int a{}; a < 2; ++a) {

			int prev_len{};

			for (const _escape& v : values[a])
				escape_value(e, M.escape_value, v, a, exact_text, prev_len, true);

		}

		return e.finish();
	}

	// decodes the frames on construction, then hands out the escape values as decode_axis asks for them
	struct _reader {

		_models& M;
		cm::bit_decoder d;
		int prev_len[2]{};

		_reader(_models& models, std::span<const u8> data, _frame_data& fd, const _side& side, size_t frames) : M{ models }, d{ data } {

			M.init(frames, side.mode);
			seed_priors(M, side);

			frame_loop(d, M, fd, side, frames, false);

		}

		float escape(const _escape& v, int a, bool exact_text) {

			d.check_end();

			return escape_value(d, M.escape_value, v, a, exact_text, prev_len[a], false);
		}

	};

}

}
