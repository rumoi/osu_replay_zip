#pragma once

// osu!taiko, osu!catch and osu!mania frames, coded like osu!standard's (orz_frames.h). Per frame: the keys (osu!mania: the
// held columns, which the game stores in x), the time delta, then what the mode's position model leaves open: nothing
// for osu!taiko (the mouse sits at the side of the last pressed key) and osu!mania (y is the scroll speed, its changes
// are listed up front), the catcher's x for osu!catch. Frames a model can not give are escaped with their values up front.

#include <algorithm>
#include <bit>
#include <span>
#include <vector>

#include "orz_state.h"
#include "orz_tables.h"
#include "orz_write.h"
#include "orz_time.h"
#include "orz_frames.h"

namespace orz {

namespace modes {

	// the frames form of an e_mode uses_modes_coder sends here
	FORCEINLINE kind mode_of(e_mode m) {
		return m == e_mode::mania ? mania : game_mode(m) == 1 ? taiko : fruits;
	}

	// osu!mania x holds the held columns as bits, up to 18 (9K co-op)
	constexpr int MAX_COLUMNS{ 18 };

	// y of every osu!taiko and osu!catch frame
	constexpr float FIELD_Y{ 9999.f };

	constexpr double HYPER_MIN_RATIO{ 1.3 };

	// valid catcher residuals are far below 2^40, only damaged files reach it
	constexpr i64 MAX_RES{ 1ll << 40 };

	FORCEINLINE float taiko_x(u32 keys) {
		return (keys & 3) ? 0.f : ((keys & 12) ? 640.f : 320.f);
	}

	// osu!catch: catcher px per ms, dashing on key 1
	FORCEINLINE double walk_speed(u32 keys) {
		return (keys & 1) ? 1.0 : 0.5;
	}

	// dotnet::text_ord, negative positions are escaped
	FORCEINLINE bool text_ordinal(float x, bool exact_text, i64& ord) {
		return !std::signbit(x) && dotnet::text_ord(x, exact_text, ord);
	}

	inline bool escaped(kind mode, const _osr_frame& f, bool exact_text) {

		i64 ord;

		switch (mode) {
		case taiko: return !same_bits(f.x, taiko_x(f.keys)) || !same_bits(f.y, FIELD_Y);
		case fruits: return !same_bits(f.y, FIELD_Y) || !text_ordinal(f.x, exact_text, ord);
		default: return same_bits(f.x, POS_DEFAULT_X) && same_bits(f.y, POS_DEFAULT_Y);
		}

	}

	struct _side {
		kind mode{};
		std::vector<int> table;          // the replay's delta table
		frame_time::_time_grid grid{};
		int bits{ frames::KEY_BITS };    // keys modelled one by one (osu!mania: columns)
		bool exact_text{};
	};

	// escaped frames and their positions (count, then per frame the gap to it and whether it sits at the start position,
	// else x and y raw), false when they do not fit the replay
	template<typename Coder>
	bool code_escapes(Coder& c, _osr& R, std::vector<u8>& escaped, bool encoding) {

		const size_t n{ R.size() };

		u64 count{};

		for (const u8 e : escaped)
			count += e;

		count = frames::gamma(c, count + 1) - 1;

		if (count > n)
			return false;

		size_t f{};

		for (u64 k{}; k < count; ++k) {

			size_t next{ f };

			while (encoding && !escaped[next])
				++next;

			const u64 gap{ frames::gamma(c, next - f + 1) - 1 };

			if (gap >= n - f)
				return false;

			auto& r{ R[f + gap] };

			escaped[f + gap] = 1;

			if (!c.code(encoding && same_bits(r.x, POS_DEFAULT_X) && same_bits(r.y, POS_DEFAULT_Y), 32768)) {

				const float x{ float_from_bits((u32)frames::raw(c, std::bit_cast<u32>(r.x), 32)) };
				const float y{ float_from_bits((u32)frames::raw(c, std::bit_cast<u32>(r.y), 32)) };

				if (!encoding) {

					r.x = x;
					r.y = y;

				}

			} else if (!encoding) {

				r.x = POS_DEFAULT_X;
				r.y = POS_DEFAULT_Y;

			}

			f += gap + 1;

		}

		return true;
	}

	// osu!mania scroll speed (y) of the frames that are not escaped: count of changes, then per change the gap and the value
	template<typename Coder>
	bool code_speeds(Coder& c, std::vector<std::pair<u32, float>>& speeds, size_t n, bool encoding) {

		const u64 count{ frames::gamma(c, speeds.size() + 1) - 1 };

		if (count > n)
			return false;

		if (!encoding)
			speeds.resize(count);

		u64 f{};

		for (auto& [at, v] : speeds) {

			f += frames::gamma(c, at - f + 1) - 1;

			if (f >= n)
				return false;

			at = (u32)f;
			v = float_from_bits((u32)frames::raw(c, std::bit_cast<u32>(v), 32));

		}

		return true;
	}

	inline std::vector<std::pair<u32, float>> speed_changes(const _osr& R, const std::vector<u8>& escaped) {

		std::vector<std::pair<u32, float>> s;

		u32 last{};

		for (size_t f{}; f < R.size(); ++f) {

			if (escaped[f] || std::bit_cast<u32>(R[f].y) == last)
				continue;

			last = std::bit_cast<u32>(R[f].y);
			s.push_back({ (u32)f, R[f].y });

		}

		return s;
	}

	// catcher x, as a text ordinal: whether it moved, whether in the direction of its last move, then the distance from
	// the walk / dash prediction (a hyperdash's speed carries over while the catcher keeps going)
	struct _catcher {

		bool exact_text{};
		float x{ POS_DEFAULT_X };
		i64 x_ord{ -1 }; // -1: none
		double hyper{ 1.0 };
		int dir{ 1 }, since_move{ 64 }, run{};
		u32 history{}; // moved flags of the last frames, newest lowest
		bool moved{};

		// running mean of |residual| * 16, per decade of x (text ordinals per px)
		i64 scale16[3]{ 1 << 10, 1 << 14, 1 << 18 }, prev_res{};

		FORCEINLINE double carried_mul(int d) const {
			return (moved && d == dir) ? hyper : 1.0;
		}

		FORCEINLINE float predict(int d, u32 keys, int delta) const {

			const double from{ std::isfinite(x) ? (double)x : (double)POS_DEFAULT_X };

			return (float)std::clamp(from + d * walk_speed(keys) * carried_mul(d) * delta, 0.0, 512.0);
		}

		void stay() {

			moved = false;
			hyper = 1.0;
			since_move = std::min(since_move + 1, 64);
			history = history << 1;
			run = 0;

		}

		// escaped frame
		void set(float nx) {

			x = nx;

			if (!text_ordinal(nx, exact_text, x_ord))
				x_ord = -1;

			stay();

		}

		template<typename Coder>
		i64 residual(Coder& c, _models& M, i64 r, const u64 (&ctx)[5], int scale) {

			M.res.bind(ctx);

			const auto decide = [&](int y, u32 step) {
				return M.res.code(c, y, step, step, step);
			};

			if (decide(r == 0, 0))
				return 0;

			const int negative{ decide(r < 0, 1) };

			const u64 v{ r < 0 ? 0 - u64(r) : u64(r) };
			const int len{ v ? 63 - std::countl_zero(v) : 0 };

			// the length starts from about the bit length of the running mean residual
			const int start{ std::clamp(scale - 1, 0, 31) };

			int n{};

			if (start == 0 || decide(len >= start, 2)) {

				n = start;

				while (n < 62 && decide(len > n, 3 + (u32)std::min(n, 31)))
					++n;

			} else {

				n = start - 1;

				while (n > 0 && !decide(len >= n, 35 + (u32)std::min(n, 28)))
					--n;

			}

			u64 out{ 1 };

			for (int b{ n - 1 }, k{}; b >= 0; --b, ++k) {

				const int bit{ (int)((v >> b) & 1) };

				if (k < 2) {

					const u64 id{ (u64)k * 4 + (out & ((u64(1) << k) - 1)) };
					const u64 cx[2]{ (ctx[0] * 64 + (u64)n) * 8 + id, ((u64)n * 8 + id) * 2 + 1 };

					out = (out << 1) | (u64)M.res_bits.code(c, bit, cx, (size_t)id, (size_t)id);

				} else
					out = (out << 1) | (u64)c.code(bit, 32768);

			}

			return negative ? -(i64)out : (i64)out;
		}

		// the frame's x (decoding: fills r.x), true when the catcher moved
		template<typename Coder>
		bool code(Coder& c, _models& M, _osr_frame& r, bool kc, bool encoding) {

			i64 want{};

			if (encoding)
				text_ordinal(r.x, exact_text, want); // not escaped, so it has one

			const u64 dt{ frames::delta_bucket(r.delta) };
			const u64 wall{ x_ord == -1 ? 2u : (u64)(x <= 0.f || x >= 512.f) };
			const u64 mctx[6]{
				(((u64)moved * 2 + kc) * 64 + dt) * 2 + (r.keys & 1),
				(u64)std::min(since_move, 63) * 32 + (u64)std::min(run, 31),
				((u64)(history & 255) * 2 + kc) * 2 + (r.keys & 1),
				((u64)(history & 15) * 64 + dt) * 3 + wall,
				((u64)(r.keys & 31) * 32 + (u64)std::min(run, 31)) * 2 + moved,
				(u64)std::min(frames::bit_length(prev_res), 40) * 64 + (u64)std::min(since_move, 63),
			};

			const size_t mset{ (size_t)moved * 32 + (size_t)std::min(run, 15) * 2 + kc };

			if (!M.moved.code(c, encoding && want != x_ord, mctx, mset, mset)) {

				if (!encoding)
					r.x = x;

				stay();

				return false;
			}

			const float wx{ encoding ? dotnet::from_text_ord(want, exact_text) : 0.f };

			const u64 dctx[4]{
				((u64)moved * 3 + wall) * 2 + (r.keys & 1),
				(u64)std::min(since_move, 63) * 32 + (u64)std::min(run, 31),
				((u64)(history & 255) * 2 + kc) * 2 + (u64)(dir > 0),
				((u64)std::min(frames::bit_length(prev_res), 40) * 2 + moved) * 64 + dt,
			};

			const size_t dset{ (size_t)moved * 8 + (size_t)std::min(run, 7) };
			const int d{ M.dir.code(c, encoding && (wx > x ? 1 : -1) == dir, dctx, dset, dset) ? dir : -dir };

			const float pred{ predict(d, r.keys, r.delta) };

			i64 pred_ord{};

			text_ordinal(pred, exact_text, pred_ord);

			const u64 decade{ u64(pred >= 10.f) + u64(pred >= 100.f) };
			const int scale{ frames::bit_length(scale16[decade] >> 4) };
			const u64 carried{ carried_mul(d) != 1.0 };
			const u64 rctx[5]{
				((((u64)moved * 2 + kc) * 2 + carried) * 3 + decade) * 2 + (r.keys & 1),
				((u64)scale * 3 + decade) * 2 + moved,
				((decade * 2 + carried) * 64 + dt) * 2 + (r.keys & 1),
				((u64)std::min(frames::bit_length(prev_res), 40) * 2 + (prev_res < 0)) * 3 + decade,
				((u64)std::min(run, 31) * 64 + (u64)std::min(since_move, 63)) * 3 + decade,
			};

			const i64 res{ std::clamp(residual(c, M, encoding ? want - pred_ord : 0, rctx, scale), -MAX_RES, MAX_RES) };
			const i64 nx_ord{ pred_ord + res };
			const float nx{ dotnet::from_text_ord(nx_ord, exact_text) };

			if (!encoding)
				r.x = nx;

			// a move much faster than walking / dashing is a hyperdash, which keeps its speed
			const double ratio{ std::abs((double)nx - (double)x) / walk_speed(r.keys) / std::max(r.delta, 1) };
			const bool inside{ nx > 0.f && nx < 512.f };

			hyper = (ratio > HYPER_MIN_RATIO && inside && r.delta >= 4 && std::isfinite(ratio)) ? ratio : 1.0;

			scale16[decade] += ((std::min<i64>(res < 0 ? -res : res, 1 << 30) << 4) - scale16[decade]) >> 3;
			prev_res = res;

			x = nx;
			x_ord = nx_ord;
			moved = true;
			dir = d;
			since_move = 0;
			history = (history << 1) | 1;
			++run;

			return true;
		}

	};

	// encoding reads R, decoding (encoding false) fills it; escaped frames' positions are known already
	template<typename Coder>
	void frame_loop(Coder& c, _models& M, _osr& R, const std::vector<u8>& escaped, const std::vector<std::pair<u32, float>>& speeds,
		const _side& side, bool encoding) {

		const size_t n{ R.size() };

		std::vector<int> delta(n);

		frames::_key_coder keys{ side.bits };
		frames::_time_coder time{ side.table, side.grid };
		_catcher catcher{ side.exact_text };

		i64 now{};
		u32 pk{}; // keys before the frame (osu!mania: the held columns)
		bool pkc{}; // the keys changed in the previous frame
		u64 stayed[3]{ 2, 2, 2 }; // whether the keys stayed in the last three frames (2 before the first)
		size_t speed_at{};
		float speed{};

		for (size_t f{}; f < n; ++f) {

			if ((f & 1023) == 0)
				c.check_end();

			auto& r{ R[f] };
			const bool esc{ escaped[f] != 0 };
			const u64 phase{ side.grid.bin(now) };

			// osu!mania keys are the held columns, which escaped frames leave as they are
			const u32 want{ encoding ? (side.mode == mania ? (u32)r.x : r.keys) : 0u };
			const u32 k{ side.mode == mania && esc ? pk : keys.code(c, M.keys_time, pk, want, now) };

			const bool kc{ k != pk };

			const u64 stays{ side.mode == taiko ? (stayed[0] * 3 + stayed[1]) * 3 + stayed[2] : 0 };

			delta[f] = time.code(c, M.keys_time.time, std::span{ delta }.first(f),
				encoding ? r.delta : 0, phase, kc, pkc, stays);

			stayed[2] = stayed[1];
			stayed[1] = stayed[0];
			stayed[0] = !kc;

			if (!encoding) {

				r.delta = delta[f];
				r.keys = side.mode == mania ? 0u : k;

			}

			if (side.mode == fruits) {

				if (esc)
					catcher.set(r.x);
				else {

					catcher.code(c, M, r, kc, encoding);

					if (!encoding)
						r.y = FIELD_Y;

				}

			} else if (!encoding && !esc) {

				if (side.mode == taiko) {

					r.x = taiko_x(k);
					r.y = FIELD_Y;

				} else {

					while (speed_at < speeds.size() && speeds[speed_at].first <= f)
						speed = speeds[speed_at++].second;

					r.x = (float)k;
					r.y = speed;

				}

			}

			now += delta[f];

			keys.update(pk, k, now);

			pk = k;
			pkc = kc;

		}

	}

	// what the frames are coded against: the delta table and time grid, which frames are escaped and osu!mania's
	// scroll speeds
	struct _prepared {
		_side side;
		u32 grid{};
		std::vector<u8> escaped;
		std::vector<std::pair<u32, float>> speeds;
	};

	inline _prepared prepare(const _osr& R, kind mode, bool exact_text) {

		_prepared p{ { mode } };

		p.escaped.resize(R.size());

		for (size_t f{}; f < R.size(); ++f)
			p.escaped[f] = escaped(mode, R[f], exact_text);

		const auto delta{ frame_time::deltas(R) };

		p.grid = frame_time::pick_grid(delta);
		p.side.table = frame_time::build_table(delta);
		p.side.grid = frame_time::_time_grid{ p.grid };
		p.side.exact_text = exact_text;

		if (mode == mania) {

			u32 columns{};

			for (size_t f{}; f < R.size(); ++f)
				if (!p.escaped[f])
					columns |= (u32)R[f].x;

			p.side.bits = std::max(1, (int)std::bit_width(columns));
			p.speeds = speed_changes(R, p.escaped);

		}

		return p;
	}

	// osu!taiko, osu!catch or osu!mania, as the gamemode says
	void encode(const _osr& r, orz_state& state) {

		const kind mode{ mode_of(state.mode()) };

		// osu!mania keeps the held columns in x (keys stay 0), a replay the column form can not hold is stored as it is
		if (mode == mania) {

			for (const auto& f : r)
				if (f.keys || !(f.x >= 0.f && f.x < (float)(1u << MAX_COLUMNS)) || f.x != std::floor(f.x))
					return write_verbatim(state);

		}

		_osr R{ r };

		auto [side, grid, esc, speeds]{ prepare(R, mode, state.exact_text()) };

		// frames: delta table and time grid, from the next byte the context mixed frames
		bit_stream head{};

		frame_time::write_table(head, side.table, grid);

		_models& M{ state.tables.modes };

		M.init(R.size(), mode);
		M.seed(mode);

		cm::bit_encoder e{};

		code_escapes(e, R, esc, true);

		if (mode == mania) {

			frames::gamma(e, (u64)side.bits);
			code_speeds(e, speeds, R.size(), true);

		}

		frame_loop(e, M, R, esc, speeds, side, true);

		add_to_u8(head.data, e.finish());

		add_container(state.file, orz_tag::frames, head.data);
		finish_orz_file((u32)r.size(), frames_crc(r, state.seed), state);

	}

	bool decode(u32 encode_info, std::span<const u8> data, std::vector<_osr_frame>& R, _table_memory& tables) {

		_bit_view head{ data };

		_side side{ mode_of(get_e_mode(encode_info)) };

		if (!frame_time::read_table(head, side.table, side.grid))
			return false;
		side.exact_text = (encode_info & ORZ_ENCODE_INFO::text_shortest) != 0;

		cm::bit_decoder d{ head.get_tail() };

		std::vector<u8> esc(R.size());
		std::vector<std::pair<u32, float>> speeds;

		if (!code_escapes(d, R, esc, false))
			return false;

		if (side.mode == mania) {

			side.bits = (int)std::min<u64>(frames::gamma(d, 0), MAX_COLUMNS);

			if (!code_speeds(d, speeds, R.size(), false))
				return false;

		}

		_models& M{ tables.modes };

		M.init(R.size(), side.mode);
		M.seed(side.mode);

		frame_loop(d, M, R, esc, speeds, side, false);

		return true;
	}

}

}
