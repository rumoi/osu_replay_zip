#pragma once

#include <algorithm>
#include <cmath>
#include <span>
#include <vector>

#include "orz_shared.h"

namespace orz {

namespace frame_time {

	// a grid period of num / den ms, num 0 for none
	struct _grid_period {
		u32 num, den;
	};

	// none, then 60 Hz at 1x, DT, HT, 30 Hz and 50 Hz
	constexpr _grid_period GRID_PERIODS[]{ { 0, 1 }, { 50, 3 }, { 25, 1 }, { 25, 2 }, { 100, 3 }, { 20, 1 } };
	constexpr u32 GRID_PERIOD_COUNT{ (u32)std::size(GRID_PERIODS) }, GRID_BINS{ 25 };

	constexpr u32 GRID_CUSTOM{ GRID_PERIOD_COUNT }, GRID_CUSTOM_MAX{ 0xFFFF };
	
	struct _time_grid {

		_fast_divisor num; // 0 means no grid
		u64 den{}, high{};

		_time_grid() = default;

		explicit _time_grid(u32 grid) : _time_grid{ GRID_PERIODS[grid < GRID_PERIOD_COUNT ? grid : 0] } {}

		explicit _time_grid(_grid_period p) : den{ p.den } {

			if (p.num == 0)
				return;

			num = _fast_divisor{ p.num };
			high = (u64(1) << 32) % p.num;

		}

		u64 bin(i64 now) const {

			if (num.d == 0)
				return 0;

			const u64 a{ now < 0 ? 0 - (u64)now : (u64)now };

			u64 r{ num.mod((u32)a) };

			if (a >> 32)
				r = num.mod((u32)(num.mod((u32)(a >> 32)) * high + r));

			if (now < 0 && r)
				r = num.d - r;

			return num.div((u32)(num.mod((u32)(r * den)) * GRID_BINS));
		}

	};

	FORCEINLINE std::vector<int> deltas(const std::vector<_osr_frame>& r) {

		std::vector<int> delta(r.size());

		for (size_t i{}; i < r.size(); ++i)
			delta[i] = r[i].delta;

		return delta;
	}
	
	// The delta table: the replay's distinct frame deltas, which the frames then give by their place in it. The
	// TABLE_LEAD most used come first, most used first; then the other low deltas, ascending; then the rest, most
	// used first. Only the rest can be negative. Of the low deltas, a file says that every one below its start is in
	// the table, and gives a bit each for a length of deltas from the start on.
	constexpr u32 TABLE_SMALL{ 32 }; // a leading delta below this is written in 5 bits
	constexpr size_t TABLE_LEAD{ 2 };
	constexpr u32 TABLE_START_BITS{ 5 }, TABLE_START_MAX{ (1u << TABLE_START_BITS) - 1 };
	constexpr u32 TABLE_LENGTH_MAX{ 127 }; // the most deltas a file gives a bit each for

	// a leading delta, never negative: a 1 bit and its 5 bits from 0 to 31, else a 0 bit and the delta
	inline void write_entry(bit_stream& t, int v) {

		if (t.emplace_back(v < (int)TABLE_SMALL))
			return t.write_bits<5>((u32)v);

		t.write_length_bucket<4, 0>((u32)v);

	}

	inline int read_entry(_bit_view& t) {

		if (t.read_bits<1>())
			return (int)t.read_bits<5>();

		return std::bit_cast<int>((u32)t.read_length_bucket<4, 0>());
	}

	// the bits of an entry of the rest: its sign, and its magnitude as length_bucket<4,0>
	inline u32 rest_bits(int v) {

		const u32 length{ (u32)std::bit_width(v < 0 ? 0u - (u32)v : (u32)v) };

		return 1 + 4 + (length < 15 ? 0 : 5) + (length ? length - 1 : 0);
	}

	struct _table_range {
		u32 start, length;
	};

	// the bits of a length written as length_bucket<3,0>
	inline u32 length_bits(u32 length) {

		const u32 width{ (u32)std::bit_width(length) };

		return 3 + (width < 7 ? 0 : 5) + (width ? width - 1 : 0);
	}

	// The start and length that make the table smallest. The start is as far as every delta is a leading entry or one
	// of the others. From there a delta within the length costs its bit, and an entry past it its own bits: the length
	// is the lower of two that tie.
	inline _table_range table_range(std::span<const int> leading, std::span<const int> entries) {

		u32 rest[TABLE_START_MAX + TABLE_LENGTH_MAX]{}; // the bits, as the rest, of each entry
		bool held[TABLE_START_MAX]{};

		for (const int v : leading)
			if (v >= 0 && v < (int)TABLE_START_MAX)
				held[v] = true;

		for (const int v : entries) {

			if (v < 0 || v >= (int)std::size(rest))
				continue;

			rest[v] += rest_bits(v);

			if (v < (int)TABLE_START_MAX)
				held[v] = true;

		}

		u32 start{};

		while (start < TABLE_START_MAX && held[start])
			++start;

		u32 length{};
		i64 cost{}, least{ length_bits(0) }; // against the entries from the start on all being the rest

		for (u32 k{ 1 }; k <= TABLE_LENGTH_MAX; ++k) {

			cost += 1 - (i64)rest[start + k - 1];

			if (cost + length_bits(k) < least) {

				least = cost + length_bits(k);
				length = k;

			}

		}

		return { start, length };
	}

	// counts of each value, in the order they first appear
	inline std::pair<u32, u32>& unique_add(u32 v, std::vector<std::pair<u32, u32>>& b) {

		for (auto& t : b) {

			if (t.first == v) {

				++t.second;

				return t;
			}

		}

		return b.emplace_back(v, 1);
	}

	inline std::vector<int> build_table(std::span<const int> delta) {

		std::vector<std::pair<u32, u32>> counts{};

		counts.reserve(64);

		for (const int d : delta)
			unique_add(std::bit_cast<u32>(d), counts);

		// most counted first, the smaller delta first of two counted the same
		std::sort(counts.begin(), counts.end(), [](const std::pair<u32, u32>& x, const std::pair<u32, u32>& y) {
			return x.second != y.second ? x.second > y.second : std::bit_cast<int>(x.first) < std::bit_cast<int>(y.first);
		});

		std::vector<int> table, others, small, rest;

		table.reserve(counts.size() + TABLE_LEAD);

		for (size_t i{}; i < counts.size(); ++i) {

			const int v{ std::bit_cast<int>(counts[i].first) };

			(v >= 0 && table.size() < TABLE_LEAD ? table : others).push_back(v);

		}

		// A leading entry cannot be negative. A replay with too few other deltas to lead its negative ones gets
		// deltas it does not use there.
		for (int v{}; table.size() < TABLE_LEAD && others.size(); ++v)
			if (std::find(table.begin(), table.end(), v) == table.end())
				table.push_back(v);

		const _table_range range{ table_range(table, others) };

		for (const int v : others)
			(v >= 0 && v < (int)(range.start + range.length) ? small : rest).push_back(v);

		std::sort(small.begin(), small.end());

		table.insert(table.end(), small.begin(), small.end());
		table.insert(table.end(), rest.begin(), rest.end());

		return table;
	}

	inline u32 pick_grid(std::span<const int> delta) {

		constexpr int SYM{ 48 };

		u32 counts[GRID_PERIOD_COUNT][GRID_BINS][SYM]{};

		_time_grid grids[GRID_PERIOD_COUNT];

		for (u32 g{}; g < GRID_PERIOD_COUNT; ++g)
			grids[g] = _time_grid{ g };

		i64 now{};

		for (const int d : delta) {

			const int s{ std::clamp(d, 0, SYM - 1) };

			for (u32 g{}; g < GRID_PERIOD_COUNT; ++g)
				++counts[g][grids[g].bin(now)][s];

			now += d;

		}

		u32 best{};
		double best_bits{ std::numeric_limits<double>::max() };

		for (u32 g{}; g < GRID_PERIOD_COUNT; ++g) {

			double bits{};

			for (const auto& row : counts[g]) {

				u32 total{};

				for (const u32 c : row)
					total += c;

				for (const u32 c : row)
					if (c)
						bits += c * std::log2((double)total / c) + 0.5 * std::log2((double)total);

			}

			if (bits < best_bits) {

				best_bits = bits;
				best = g;

			}

		}

		return best;
	}

	// pick_grid's estimate for one grid: the bits the deltas take with its phase as their only context
	inline double grid_bits(std::span<const int> delta, const _time_grid& grid) {

		constexpr int SYM{ 48 };

		u32 counts[GRID_BINS][SYM]{};

		i64 now{};

		for (const int d : delta) {

			++counts[grid.bin(now)][std::clamp(d, 0, SYM - 1)];

			now += d;
		}

		double bits{};

		for (const auto& row : counts) {

			u32 total{};

			for (const u32 c : row)
				total += c;

			for (const u32 c : row)
				if (c)
					bits += c * std::log2((double)total / c) + 0.5 * std::log2((double)total);

		}

		return bits;
	}

	// A grid for ticks about `period` ms apart when that is none of the listed periods: a game clock off the usual play
	// rates. The periods tried are whole 24ths of a millisecond, which holds every hundredth of the three play rates'
	// periods. num 0 when the listed grid (index `listed`) serves the deltas as well.
	inline _grid_period custom_grid(std::span<const int> delta, double period, u32 listed) {

		for (u32 g{ 1 }; g < GRID_PERIOD_COUNT; ++g)
			if (std::abs(period * (double)GRID_PERIODS[g].den / (double)GRID_PERIODS[g].num - 1.) < 0.015)
				return {};

		constexpr u32 DEN{ 24 };

		_grid_period best{};

		// a custom period has to earn its bytes in the head, and more than a fit to noise would
		double best_bits{ 0.98 * grid_bits(delta, _time_grid{ listed }) };

		// the ticks' mean spacing reads a little short where other frames fall between them
		for (u32 k{ std::max(1u, (u32)(period * 0.985 * DEN)) }; k <= (u32)(period * 1.075 * DEN) + 1 && k <= GRID_CUSTOM_MAX; ++k) {

			const double bits{ grid_bits(delta, _time_grid{ _grid_period{ k, DEN } }) };

			if (bits < best_bits) {

				best_bits = bits;
				best = { k, DEN };
			}

		}

		return best;
	}

	// the delta table and time grid, at the front of the stream holding the times (custom: the period of GRID_CUSTOM)
	inline void write_table(bit_stream& t, const std::vector<int>& table, u32 grid, _grid_period custom = {}) {

		t.small_write<5>((u32)table.size());

		size_t k{};

		for (; k < std::min(TABLE_LEAD, table.size()); ++k)
			write_entry(t, table[k]);

		// the low deltas come next, ascending: all of those below the start, and a bit for each of a length from it on
		const _table_range range{ table_range(std::span{ table }.first(k), std::span{ table }.subspan(k)) };

		t.write_bits<TABLE_START_BITS>(range.start);
		t.write_length_bucket<3, 0>(range.length);

		for (int v{}; v < (int)range.start; ++v)
			k += k < table.size() && table[k] == v;

		for (int v{ (int)range.start }; v < (int)(range.start + range.length); ++v)
			if (t.emplace_back(k < table.size() && table[k] == v))
				++k;

		// the rest: a sign bit and the magnitude
		for (; k < table.size(); ++k) {

			t.push_back(table[k] < 0);
			t.write_length_bucket<4, 0>(table[k] < 0 ? 0u - (u32)table[k] : (u32)table[k]);

		}

		t.write_bits<3>(grid);

		if (grid == GRID_CUSTOM) {

			// each less 1: neither is ever 0
			t.write_bucket(custom.num - 1, 7);
			t.write_bucket(custom.den - 1, 3);

		}

	}

	// from t's cursor, t ends past it
	inline bool read_table(_bit_view& t, std::vector<int>& table, _time_grid& grid) {

		const size_t n{ t.small_read<5>() };

		table.clear();

		while (table.size() < std::min(TABLE_LEAD, n) && t.in < t.max_size())
			table.push_back(read_entry(t));

		const size_t leading{ table.size() };

		const int start{ (int)t.read_bits<TABLE_START_BITS>() };
		const u64 length{ t.read_length_bucket<3, 0>() };

		if (length > TABLE_LENGTH_MAX)
			return false;

		// every delta below the start that is not a leading entry
		for (int v{}; v < start && table.size() < n; ++v)
			if (std::find(table.begin(), table.begin() + leading, v) == table.begin() + leading)
				table.push_back(v);

		for (int v{ start }; v < start + (int)length; ++v)
			if (t.read_bits<1>() && table.size() < n)
				table.push_back(v);

		while (table.size() < n && t.in < t.max_size()) {

			const bool negative{ t.read_bits<1>() != 0 };
			const u32 m{ (u32)t.read_length_bucket<4, 0>() };

			table.push_back(std::bit_cast<int>(negative ? 0u - m : m));

		}

		const u32 code{ (u32)t.read_bits<3>() };

		if (code < GRID_PERIOD_COUNT) {

			grid = _time_grid{ code };

			return true;
		}

		if (code != GRID_CUSTOM)
			return false;

		// each is stored less 1, so neither can be 0
		const u64 num{ (u64)t.read_bucket<7>() + 1 }, den{ (u64)t.read_bucket<3>() + 1 };

		if (num > GRID_CUSTOM_MAX || den > GRID_CUSTOM_MAX)
			return false;

		grid = _time_grid{ _grid_period{ (u32)num, (u32)den } };

		return true;
	}

	// Audio time against real time by the 60 Hz ticks, 0 with too few clean ticks to say. The game records a frame on the
	// first update after each 60th of a second of real time, so ticks are 16.67 ms of audio apart at normal speed, 25
	// with double time, 12.5 with half time.
	inline float tick_rate(const _osr& r) {

		constexpr int LO{ 8 }, HI{ 45 }; // audio ms a tick can be apart

		const size_t n{ r.size() };

		size_t by_delta[HI + 2]{};

		for (const auto& f : r)
			if (f.delta >= LO && f.delta <= HI)
				++by_delta[f.delta];

		// the commonest spacing, over three neighbouring values
		const auto around = [&](int d) { return by_delta[d - 1] + by_delta[d] + by_delta[d + 1]; };

		int mode{ LO + 1 };

		for (int d{ LO + 2 }; d < HI; ++d)
			mode = around(d) > around(mode) ? d : mode;

		// Ticks keep to their own grid whatever else is recorded. Where the game's frames are long a tick falls on one
		// of the two frame counts around the period, and the mean spacing is still the period. A frame recorded between
		// two ticks splits their spacing in two, one part short: a spacing with a short neighbour is not counted.
		const int lo{ (int)(0.6 * mode) }, hi{ (int)(1.65 * mode) };

		const auto whole = [&](size_t i) { return r[i].delta >= lo && r[i].delta <= hi; };

		const auto clean = [&](size_t i) { return whole(i - 1) && whole(i) && whole(i + 1); };

		double sum{};
		size_t count{};

		for (size_t i{ 1 }; i + 1 < n; ++i) {

			if (!clean(i))
				continue;

			sum += (double)r[i].delta;
			++count;
		}

		if (count < 50)
			return 0.f;

		return (float)(sum / (double)count / (1000. / 60.));
	}

	// How fast audio time ran against real time, 0 where the replay does not show it. play_rate is the rate the replay's
	// mods state (0: unknown); grid_ms is the period of the grid the times are coded on, 0 for none.
	inline float clock_rate(const _osr& r, float play_rate, double grid_ms) {

		float rate{ tick_rate(r) };

		// the ticks' mean spacing reads up to a few percent short where other frames fall between them: the grid the
		// times are coded on is their exact period where the two agree
		if (const double on_grid{ grid_ms / (1000. / 60.) }; rate >= 0.92 * on_grid && rate <= 1.015 * on_grid)
			rate = (float)on_grid;

		// a known play rate the ticks agree with is exact
		for (const float known : { play_rate, 1.f, 1.5f, 0.75f })
			if (known > 0.f && std::abs(rate / known - 1.f) < 0.015f)
				return known;

		return rate;
	}

}

}
