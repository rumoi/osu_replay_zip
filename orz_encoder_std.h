#pragma once

#include <algorithm>
#include <climits>
#include <array>
#include <unordered_map>
#include <vector>

#include "orz_state.h"
#include "orz_write.h"
#include "orz_time.h"
#include "orz_frames.h"
#include "orz_device_std.h"
#include "orz_setup.h"

namespace orz {

namespace standard {

	namespace repeat {

		inline std::vector<u8> flags(const _osr& r) {

			std::vector<u8> repeated(r.size());

			for (size_t i{ 1 }; i < r.size(); ++i)
				repeated[i] = same_bits(r[i].x, r[i - 1].x) && same_bits(r[i].y, r[i - 1].y);

			return repeated;
		}

		inline std::vector<int> sample_dt(std::span<const int> delta, const std::vector<u8>& repeated) {

			std::vector<int> dt;

			i64 elapsed{};

			for (size_t i{}; i < delta.size(); ++i) {

				elapsed += delta[i];

				if (repeated[i])
					continue;

				dt.push_back((int)std::clamp<i64>(elapsed, INT_MIN, INT_MAX));

				elapsed = 0;

			}

			return dt;
		}

		inline std::vector<u32> sample_of(const std::vector<u8>& repeated) {

			std::vector<u32> s(repeated.size());

			for (size_t i{ 1 }; i < repeated.size(); ++i)
				s[i] = s[i - 1] + !repeated[i];

			return s;
		}

	}

	inline u32 fold_keys(u32 k) {
		return k ^ ((k >> 2) & 3);
	}

	// what every candidate model codes alike: times, repeats, keys
	struct _shared_streams {
		std::vector<int> delta;
		std::vector<u8> repeated;
		std::vector<u32> sample;
		std::vector<u32> keys; // folded, K1 / K2 imply M1 / M2
		std::vector<int> table;
		u32 grid{};
		frame_time::_grid_period custom{}; // the period when grid is GRID_CUSTOM
		bool std_default_pos{};
		bool std_default_time{};

		frame_time::_grid_period period() const {

			return grid == frame_time::GRID_CUSTOM ? custom : frame_time::GRID_PERIODS[grid];
		}

		frame_time::_time_grid time_grid() const {

			return frame_time::_time_grid{ period() };
		}

	};

	inline bool std_default_pos_of(const _osr& r) {

		return r.size() > 2 && same_bits(r[0].x, POS_DEFAULT_X) && same_bits(r[0].y, POS_DEFAULT_Y) &&
			same_bits(r[1].x, POS_DEFAULT_X) && same_bits(r[1].y, POS_DEFAULT_Y);
	}

	inline _shared_streams shared_streams(const _osr& r, bool std_default_pos, bool std_default_time) {

		_shared_streams sh{};

		sh.delta = frame_time::deltas(r);
		sh.std_default_pos = std_default_pos;
		sh.std_default_time = std_default_time;

		// the first two deltas are not coded with std_default_time
		sh.table = frame_time::build_table(std::span{ sh.delta }.subspan(std_default_time ? 2 : 0));
		sh.grid = frame_time::pick_grid(sh.delta);

		// a game clock off the usual play rates has its ticks on none of the listed grids: one of their own period
		if (const float rate{ frame_time::tick_rate(r) }; rate > 0.f) {

			sh.custom = frame_time::custom_grid(sh.delta, (double)rate * (1000. / 60.), sh.grid);

			if (sh.custom.num)
				sh.grid = frame_time::GRID_CUSTOM;

		}

		sh.repeated = repeat::flags(r);
		sh.sample = repeat::sample_of(sh.repeated);

		sh.keys.resize(r.size());

		for (size_t i{}; i < r.size(); ++i)
			sh.keys[i] = fold_keys(r[i].keys);

		return sh;
	}

	using dspace::_native_grid;

	inline i64 grid_value(const _native_grid& g, i64 j) {

		return (i64)((u64)g.A * (u64)j + (u64)g.C) >> 32;
	}

	inline bool grid_index(const _native_grid& g, i64 L, i64& j) {

		const i64 num{ (L << 32) - g.C };

		j = num >= 0 ? (i64)(((u64)num + g.A - 1) / g.A) : -(i64)((u64)(-num) / g.A);

		return grid_value(g, j) == L;
	}

	inline _native_grid fit_grid(const std::vector<i64>& K) {

		std::vector<i64> d{ K };

		std::sort(d.begin(), d.end());
		d.erase(std::unique(d.begin(), d.end()), d.end());

		// a few escapes can widen the range well past 65535
		if (d.size() < 64 || d.front() <= -(1 << 24) || d.back() >= (1 << 24))
			return {};

		std::unordered_map<i64, int> gaps;

		for (size_t i{ 1 }; i < d.size(); ++i)
			++gaps[d[i] - d[i - 1]];

		std::vector<std::pair<int, i64>> common;

		for (const auto& [k, n] : gaps)
			common.push_back({ n, k });

		std::sort(common.begin(), common.end(), std::greater<>{});
		common.resize(std::min<size_t>(common.size(), 6));

		const auto covered = [&](double st) {

			i64 n{};

			for (const auto& [k, c] : gaps) {

				const double m{ std::round((double)k / st) };

				if (m >= 1 && std::abs((double)k - m * st) <= 0.5 + 0.01 * (double)k)
					n += c;

			}

			return n;
		};

		std::vector<std::pair<double, i64>> cand;
		std::vector<double> pairs;

		for (const auto& [n1, g1] : common) {

			const int lo_n{ gaps.count(g1 - 1) ? gaps[g1 - 1] : 0 }, hi_n{ gaps.count(g1 + 1) ? gaps[g1 + 1] : 0 };
			const i64 g2{ lo_n >= hi_n ? g1 - 1 : g1 + 1 };
			const int n2{ std::max(lo_n, hi_n) };
			const double pair{ (double)(g1 * n1 + g2 * n2) / (n1 + n2) };

			pairs.push_back(pair);

			for (int k{ 1 }; k <= 4; ++k)
				if (pair / k >= 1.5)
					cand.push_back({ pair / k, covered(pair / k) });

		}

		i64 most{};

		for (const auto& c : cand)
			most = std::max(most, c.second);

		double s{};

		for (const auto& [st, n] : cand)
			if (n * 10 >= most * 9)
				s = std::max(s, st);

		// a step the gaps agree on: the device's own lattice, one count a step
		const bool coarse{ s >= 1.5 && most * 2 >= (i64)d.size() - 1 };

		struct _fitter {

			const std::vector<i64>& d;

			double span{}, s{}, phase{};

			std::vector<double> few, many;

			std::vector<i64> on, j;

			std::vector<size_t> up, dn;

			std::vector<double> sample(size_t n) const {

				std::vector<double> u;

				for (size_t i{}, stride{ std::max<size_t>(1, d.size() / n) }; i < d.size(); i += stride)
					u.push_back((double)(d[i] - d.front()) / span);

				return u;
			}

			void sweep(double reach_steps, double q_step, const std::vector<double>& u, double least = 1.2) {

				constexpr int BINS{ 256 };

				const double n0{ span / s };
				const int reach{ (int)(reach_steps / q_step) };

				int hist[BINS], best{ -1 };
				double best_s{ s };

				for (int q{ -reach }; q <= reach; ++q) {

					const double N{ n0 + q * q_step }, st{ span / N };

					// a wide reach can pass zero steps
					if (N <= 0 || st < least)
						continue;

					std::fill(std::begin(hist), std::end(hist), 0);

					for (const double x : u) {

						const double f{ x * N };

						++hist[std::min(BINS - 1, (int)((f - std::floor(f)) * BINS))];

					}

					const int w{ std::max(1, (int)(BINS / st)) };

					int sum{};

					for (int b{}; b < w; ++b)
						sum += hist[b];

					for (int b{}; b < BINS; ++b) {

						if (sum > best) {

							best = sum;
							best_s = st;
							phase = (b + w * 0.5) / BINS * st;

						}

						sum += hist[(b + w) % BINS] - hist[b];

					}

				}

				s = best_s;
			}

			void hulls() {

				const auto cross = [&](size_t o, size_t a, size_t b) {

					return (j[a] - j[o]) * (on[b] - on[o]) - (on[a] - on[o]) * (j[b] - j[o]);
				};

				up.clear();
				dn.clear();

				for (size_t i{}; i < on.size(); ++i) {

					while (up.size() >= 2 && cross(up[up.size() - 2], up.back(), i) >= 0)
						up.pop_back();

					up.push_back(i);

					while (dn.size() >= 2 && cross(dn[dn.size() - 2], dn.back(), i) <= 0)
						dn.pop_back();

					dn.push_back(i);

				}

			}

			double window(double st, double& lo) const {

				double hi{ std::numeric_limits<double>::max() };

				lo = std::numeric_limits<double>::lowest();

				for (size_t i : up)
					lo = std::max(lo, (double)on[i] - (double)j[i] * st);

				for (size_t i : dn)
					hi = std::min(hi, (double)on[i] + 1. - (double)j[i] * st);

				return hi - lo;
			}

			size_t refine(bool exact) {

				double lo;

				for (int it{}; it < 3; ++it) {

					std::vector<std::pair<double, size_t>> t(d.size());
					std::vector<i64> jd(d.size());

					for (size_t i{}; i < d.size(); ++i) {

						jd[i] = std::llround(((double)(d[i] - d.front()) - phase) / s);
						t[i] = { (double)(d[i] - d.front()) - (double)jd[i] * s, i };

					}

					size_t count{ d.size() }, at{};

					if (!exact) {

						std::sort(t.begin(), t.end());

						count = 0;

						for (size_t e{}, b{}; e < t.size(); ++e) {

							while (t[e].first - t[b].first >= 1.)
								++b;

							if (e - b + 1 > count) {

								count = e - b + 1;
								at = b;

							}

						}

						std::sort(t.begin() + at, t.begin() + at + count, [](const auto& x, const auto& y) {

							return x.second < y.second;
						});

					}

					on.clear();
					j.clear();

					const i64 low{ jd[t[at].second] };

					for (size_t k{ at }; k < at + count; ++k) {

						on.push_back(d[t[k].second]);
						j.push_back(jd[t[k].second] - low);

					}

					hulls();

					double a{ s * 0.999 }, b{ s * 1.001 };

					for (int k{}; k < 40; ++k) {

						const double m1{ a + (b - a) / 3 }, m2{ b - (b - a) / 3 };

						if (window(m1, lo) < window(m2, lo))
							a = m1;
						else
							b = m2;

					}

					s = (a + b) / 2;

					// clearly no exact grid near this step
					if (exact && window(s, lo) < -1.)
						break;

				}

				return window(s, lo) > 0 ? on.size() : 0;
			}

			size_t fit_from(double st) {

				s = st;
				phase = 0;

				for (int it{}; it < 20; ++it) {

					i64 n{};

					for (size_t i{ 1 }; i < d.size(); ++i)
						n += std::max<i64>(1, std::llround((double)(d[i] - d[i - 1]) / s));

					const double s1{ span / (double)n };

					if (s1 == s)
						break;

					s = s1;

				}

				if (const size_t n{ refine(true) }; n != 0)
					return n;

				// a span step far from st is some other step
				if (std::abs(st - s) > s * 0.1)
					return 0;

				sweep(span / s * std::max(0.01, std::abs(st - s) / s + 0.005), 1., few);
				sweep(1., 0.25, many);

				size_t count{ refine(false) };

				if (count == 0)
					return 0;

				const double s0{ s }, p0{ phase };

				sweep(std::min(span / s * 0.0025, 8.), 0.125, many);

				double lo;

				if (const size_t c{ refine(false) }; c > count && window(s, lo) > 0)
					count = c;
				else {

					s = s0;
					phase = p0;
					refine(false);

				}

				return count;
			}

			// estimated bits saved per value
			double gain(size_t n) const {

				const double f{ (double)n / (double)d.size() };

				return n * 10 < d.size() * 7 ? std::numeric_limits<double>::lowest() : f * std::log2(s) - (1. - f) * 3.;
			}

		};

		_fitter f{ d, (double)(d.back() - d.front()) };

		f.few = f.sample(256);
		f.many = f.sample(2048);

		// the grid of the lattice the fitter holds, off where it holds none
		const auto grid_of = [&]() -> _native_grid {

			if (f.on.empty())
				return {};

			// Any lattice holds its share of any integers, all but the 1 - 1 / step it skips, and one under 1.5 integers a
			// count holds most. Fit to integers that are on none, the best still misses about that share; a tablet's own
			// misses a small part of it (what the device model escaped, the cursor held at an edge). Half of it divides
			// the two.
			if (f.s < 1.5 && (double)(d.size() - f.on.size()) > (double)d.size() * (1. - 1. / f.s) / 2.)
				return {};

			double lo;

			const double w{ f.window(f.s, lo) };
			const u64 A{ (u64)std::llround(f.s * 4294967296.) };

			if (w <= 0 || A == 0)
				return {};

			return { true, A, (i64)std::floor((lo + w / 2) * 4294967296.) };
		};

		if (coarse) {

			const double guess{ s };

			if (const size_t count{ f.fit_from(guess) }; count * 10 < d.size() * 9) {

				double best{ f.gain(count) }, best_s{ f.s }, best_phase{ f.phase };
				std::vector<double> tried{ guess };

				for (const double pr : pairs)
					for (int k{ 1 }; k <= 6; ++k) {

						const double st{ pr / k };

						const bool seen{ std::any_of(tried.begin(), tried.end(), [&](double t) {

							return std::abs(st - t) < t * 0.005;
						}) };

						if (st < 1.2 || seen)
							continue;

						tried.push_back(st);

						if (const double g{ f.gain(f.fit_from(st)) }; g > best) {

							best = g;
							best_s = f.s;
							best_phase = f.phase;

						}

					}

				f.s = best_s;
				f.phase = best_phase;

				if (best == std::numeric_limits<double>::lowest())
					f.on.clear();

				else if (f.refine(true) == 0)
					f.refine(false);

			}

			// The span's count of steps can settle off the gaps' own estimates: a step just under 2 leaves gaps of 2 and a
			// few of 1, and reads as one just over 2 with those left off it. The lattice at an estimate near the one
			// found stands where it holds more of the integers.
			double lo;

			const double found{ f.s };

			size_t held{ f.window(f.s, lo) > 0 ? f.on.size() : 0 };

			for (size_t k{}; k < cand.size() && held < d.size(); ++k) {

				const double st{ cand[k].first };

				const bool seen{ std::any_of(cand.begin(), cand.begin() + k, [&](const auto& c) {

					return std::abs(st - c.first) < st * 0.001;
				}) };

				if (seen || std::abs(st - found) > found * 0.05)
					continue;

				const double s0{ f.s }, phase0{ f.phase };
				const auto on0{ f.on }, j0{ f.j };
				const auto up0{ f.up }, dn0{ f.dn };

				f.s = st;
				f.sweep(std::max(8., f.span / st * 0.005), 0.25, f.few);

				size_t direct{ f.refine(true) };

				if (direct == 0)
					direct = f.refine(false);

				if (direct > held)
					held = direct;

				else {

					f.s = s0;
					f.phase = phase0;
					f.on = on0;
					f.j = j0;
					f.up = up0;
					f.dn = dn0;

				}

			}

			if (const _native_grid g{ grid_of() }; g.on)
				return g;

		}

		// A lattice under 1.5 integers a count leaves gaps of 1 and 2, which say little of its step. What shows it are the
		// integers it never lands on: one every step / (step - 1), each alone among visited ones. How far apart those are
		// gives the step, to within fine_reach counts across the span. It is looked for where the gaps agree on no
		// step, or on one that then holds no lattice (near 1.5 every gap of 1, 2 or 3 is within reach of a step).
		double fine_reach{};

		std::unordered_map<i64, int> apart;

		i64 last{};
		bool any{};

		for (size_t i{ 1 }; i + 2 < d.size(); ++i) {

			// d[i] + 1 was never visited, the two integers either side of it were
			if (d[i + 1] - d[i] != 2 || d[i] - d[i - 1] != 1 || d[i + 2] - d[i + 1] != 1)
				continue;

			if (any)
				++apart[d[i] - last];

			any = true;
			last = d[i];

		}

		i64 g1{};
		int n1{};

		for (const auto& [g, n] : apart)
			if (n > n1 || (n == n1 && g < g1)) {

				g1 = g;
				n1 = n;

			}

		const int below{ apart.count(g1 - 1) ? apart[g1 - 1] : 0 }, above{ apart.count(g1 + 1) ? apart[g1 + 1] : 0 };
		const i64 g2{ below >= above ? g1 - 1 : g1 + 1 };
		const int n2{ std::max(below, above) }, n{ n1 + n2 };

		// the lattice's own spacing stands out: integers the replay happened not to visit are any distance apart
		int other{};

		for (const auto& [g, c] : apart)
			if (std::abs(g - g1) > 1 && g < 2 * g1 - 1)
				other = std::max(other, c);

		if (n < 32 || n1 < 2 * other)
			return {};

		const double every{ (double)(g1 * n1 + g2 * n2) / n };
		const double share{ (double)n2 / n };

		s = every / (every - 1.);

		// the estimate's deviation as a share of the step: four of them across the span, in counts
		const double deviation{ std::sqrt(std::max(share * (1. - share), 1. / n) / n) / (every * (every - 1.)) };

		fine_reach = std::min(64., 2. + 4. * deviation * (double)(d.back() - d.front()) / s);

		// the lattice holds the replay's integers only where its counts stay in step across the whole span: by less
		// than the share of integers it skips, which a few samples show unless it is small
		const double skips{ 1. - 1. / s };

		f.s = s;
		f.sweep(fine_reach, skips / 4., skips < 0.15 ? f.many : f.few, 1.);

		if (f.refine(true) == 0)
			f.refine(false);

		return grid_of();
	}

	// The grid of a known step under st's integers: the lattice of that step holding the most of them. A few sit off
	// any (the cursor held at an edge, a frame of another device) and are left to escape; off when more than a
	// hundredth do.
	inline _native_grid grid_of_step(const dspace::axis_streams& st, double step) {

		if (!(step > 1.) || !(step < 65536.) || st.K.size() < 64)
			return {};

		std::vector<u8> escaped(st.K.size());

		for (const auto& e : st.escapes)
			escaped[e.first] = 1;

		// where each integer the replay visits sits within a step, however long the cursor stayed on it
		std::vector<i64> visited;

		visited.reserve(st.K.size());

		for (size_t i{}; i < st.K.size(); ++i)
			if (!escaped[i] && st.K[i] < (1 << 24))
				visited.push_back(st.K[i]);

		std::sort(visited.begin(), visited.end());
		visited.erase(std::unique(visited.begin(), visited.end()), visited.end());

		if (visited.empty() || visited.front() <= -(1 << 24))
			return {};

		const i64 low{ visited.front() };

		std::vector<double> at(visited.size());

		for (size_t i{}; i < visited.size(); ++i) {

			const double q{ (double)(visited[i] - low) / step };

			at[i] = q - std::floor(q);

		}

		std::sort(at.begin(), at.end());

		// one lattice's integers sit within an integer of each other there: the stretch that wide holding the most of
		// them starts at the lattice's phase
		const double width{ 1. / step };
		const size_t n{ at.size() };

		size_t most{}, first{};

		for (size_t b{}, e{}; b < n; ++b) {

			e = std::max(e, b);

			while (e < b + n && at[e % n] + (e >= n ? 1. : 0.) - at[b] < width)
				++e;

			if (e - b > most) {

				most = e - b;
				first = b;

			}

		}

		if (most * 100 < n * 99)
			return {};

		const double phase{ at[first] };

		const u64 A{ (u64)std::llround(step * 4294967296.) };

		i64 r_low{ std::numeric_limits<i64>::max() }, r_high{ std::numeric_limits<i64>::min() }, j_low{ std::numeric_limits<i64>::max() };

		for (const i64 k : visited) {

			const double q{ (double)(k - low) / step - phase };
			const double whole{ std::floor(q + 1e-9) };

			// off the lattice
			if (q - whole >= width)
				continue;

			// visited is in order, so the first integer on the lattice has the lowest index
			const i64 j{ (i64)whole };

			j_low = std::min(j_low, j);

			const i64 r{ (i64)((u64)k << 32) - (i64)(A * (u64)(j - j_low)) };

			r_low = std::min(r_low, r);
			r_high = std::max(r_high, r);

		}

		// value(j) = (A * j + C) >> 32 gives every integer back when C is in [r_high, r_low + 2^32)
		if (r_high - r_low >= (1ll << 32))
			return {};

		return { true, A, r_high + ((1ll << 32) - 1 - (r_high - r_low)) / 2 };
	}

	inline float grid_field(const dspace::_axis& a, i64 L) {
		return a.param.device.kind == dspace::axis_mode::rational ? dspace::rational_value(a.param, L) : dspace::absolute_field(a, L);
	}

	inline dspace::axis_streams grid_streams(const dspace::axis_params& p, const dspace::axis_streams& st, std::span<const float> vals,
		bool exact_text, const _native_grid& g) {

		const dspace::_axis a{ p };

		dspace::axis_streams o{};

		o.K.resize(st.K.size());
		o.slack = st.slack;
		o.decoded.resize(st.K.size());

		bool held{};
		float last{};
		i64 prev{};

		for (size_t i{}; i < st.K.size(); ++i) {

			const i64 L{ st.K[i] };

			i64 j;

			// the grid value below may be nearer
			if (!grid_index(g, L, j) && j > 0 && L - grid_value(g, j - 1) < grid_value(g, j) - L)
				--j;

			j = std::max<i64>(j, 0);
			o.K[i] = j;

			const i64 Ls{ grid_value(g, j) };
			const i64 dk{ Ls - prev };

			prev = Ls;

			const dotnet::text_bounds b{ vals[i], exact_text };

			if (held && dk == 0) {

				if (b.contains(last)) {

					o.decoded[i] = last;

					continue;
				}

			} else if (const float v{ grid_field(a, Ls) }; b.contains(v)) {

				held = false;
				last = v;
				o.decoded[i] = v;

				continue;
			}

			// mostly one native count off the grid value
			float x{ vals[i] };

			for (const i64 n : { Ls - 1, Ls + 1 })
				if (const float v{ grid_field(a, n) }; b.contains(v)) {

					x = v;

					break;
				}

			o.escapes.push_back({ (u32)i, x });
			o.escape_pred.push_back(grid_field(a, Ls));

			held = true;
			last = x;
			o.decoded[i] = x;

		}

		return o;
	}

	// the values one native count either side of grid value L
	inline void grid_near(const dspace::axis_params& p, i64 L, float (&near)[2]) {

		const dspace::_axis a{ p };

		near[0] = grid_field(a, L - 1);
		near[1] = grid_field(a, L + 1);

	}

	// axis_cost of an axis on a grid. A position one native count off its grid value is coded as the side it is on
	// (grid_near): a few bits, where axis_cost would count an escaped value.
	inline size_t grid_cost(const dspace::axis_params& p, const _native_grid& g, const dspace::axis_streams& st, std::span<const int> dt,
		int axis, bool exact_text) {

		const dspace::_axis a{ p };

		dspace::axis_streams far{};

		far.K = st.K;
		far.slack = st.slack;

		size_t near{};

		for (size_t j{}; j < st.escapes.size(); ++j) {

			const i64 L{ grid_value(g, st.K[st.escapes[j].first]) };
			const float x{ st.escapes[j].second };

			if (same_bits(x, grid_field(a, L - 1)) || same_bits(x, grid_field(a, L + 1))) {

				++near;

				continue;
			}

			far.escapes.push_back(st.escapes[j]);
			far.escape_pred.push_back(st.escape_pred[j]);

		}

		return axis_cost(far, dt, axis, exact_text) + 4 * near;
	}

	// after the autopilot parameters in the head of the frames stream, per axis
	inline void write_grid(const _native_grid& g, bit_stream& o) {

		o.push_back(g.on);

		if (!g.on)
			return;

		o.write_bits<48>(g.A);
		o.write_bits<64>((u64)g.C);

	}

	inline bool read_grid(_bit_view& in, _native_grid& g) {

		g = {};
		g.on = in.read_bits<1>() != 0;

		if (!g.on)
			return true;

		g.A = in.read_bits<48>();
		g.C = (i64)in.read_bits<64>();

		return g.A != 0;
	}

	// autopilot parameters, after the device model in the head of the frames stream; whether there are any is a flag
	inline void write_autopilot(const autopilot::_params& ap, bit_stream& o) {

		if (!ap.on)
			return;

		o.small_write<2>(ap.rate);
		dspace::write_u(ap.width, o);
		dspace::write_u(ap.height, o);

	}

	inline bool read_autopilot(_bit_view& in, autopilot::_params& ap, bool on) {

		ap = { on };

		if (!ap.on)
			return true;

		ap.rate = in.small_read<2>();
		ap.width = dspace::read_u(in);
		ap.height = dspace::read_u(in);

		return ap.rate < autopilot::RATE_COUNT && ap.width && ap.height && ap.width <= (1 << 15) && ap.height <= (1 << 15);
	}

	// window sizes tried for autopilot, the common ones first
	constexpr std::pair<u32, u32> AUTOPILOT_WINDOWS[]{
		{ 1920, 1080 }, { 2560, 1440 }, { 1366, 768 }, { 1280, 720 }, { 1600, 900 }, { 1680, 1050 }, { 1440, 900 }, { 1280, 1024 },
		{ 1024, 768 }, { 800, 600 }, { 1280, 960 }, { 1360, 768 }, { 1536, 864 }, { 2560, 1080 }, { 3440, 1440 }, { 1280, 800 },
		{ 1920, 1200 }, { 2560, 1600 }, { 1600, 1200 },
	};

	// the new positions with their times and 7 digit ordinals, for autopilot (none for shortest round-trip text)
	inline std::vector<autopilot::sample> autopilot_samples(const _osr& r, const _shared_streams& shared, bool exact_text) {

		if (exact_text)
			return {};

		std::vector<autopilot::sample> s;

		i64 t{};

		for (size_t i{}; i < r.size(); ++i) {

			t += r[i].delta;

			if (shared.repeated[i]) {

				if (!s.empty())
					s.back().stays = true;

				continue;
			}

			autopilot::sample q{ t };

			q.v[0] = r[i].x;
			q.v[1] = r[i].y;

			if (!dotnet::text_ord(q.v[0], false, q.K[0]) || !dotnet::text_ord(q.v[1], false, q.K[1]))
				return {};

			s.push_back(q);

		}

		return s;
	}

	// the window, then the speed, under which fitted moves cover the most of a stretch past the start
	inline autopilot::_params search_autopilot(std::span<const autopilot::sample> s) {

		const size_t skip{ std::min<size_t>(s.size(), 64) };
		const auto head{ s.subspan(skip, std::min<size_t>(s.size() - skip, 600)) };

		autopilot::_params best{};
		size_t most{};

		const auto attempt = [&](const autopilot::_params& p) {

			size_t c{};

			autopilot::fitter{ head, p }.fit(c);

			if (c > most) {

				most = c;
				best = p;

			}

		};

		for (const auto& [w, h] : AUTOPILOT_WINDOWS)
			attempt({ true, 0, w, h });

		for (u32 rate{ 1 }; rate < autopilot::RATE_COUNT && best.on; ++rate)
			attempt({ true, rate, best.width, best.height });

		if (most * 10 < head.size())
			best = {};

		return best;
	}

	struct _positions {

		std::vector<u8> frames; // the frames stream
		std::array<dspace::axis_streams, 2> st;
		_native_grid grid[2]{}; // the native grids the frames were coded on
		_native_grid fitted[2]{}; // the native grids that fit, coded on or not
		bool autopilot{}; // coded with the autopilot path model

		size_t size() const {
			return frames.size();
		}

	};

	inline _positions position_streams(const dspace::_model_info& m, const std::vector<float> vals[2], const std::vector<int>& dt,
		const _shared_streams& shared, orz_state& state, const autopilot::_params& ap = {}, const std::vector<autopilot::event>& moves = {}) {

		_positions p{};

		p.autopilot = ap.on;

		for (int a{}; a < 2; ++a)
			p.st[a] = dspace::encode_axis_model(m.axis[a], vals[a], dt, state.exact_text());

		// frames: delta table and device model, from the next byte the context mixed frames and escape values
		bit_stream model_head{};

		frame_time::write_table(model_head, shared.table, shared.grid, shared.custom);
		dspace::write_info(m, model_head);
		write_autopilot(ap, model_head);

		_native_grid grid[2]{};
		dspace::axis_streams on_grid[2];

		for (int a{}; a < 2; ++a)
			if (m.axis[a].seg.size() == 1 && (m.axis[a].seg[0].p.device.kind == dspace::axis_mode::absolute ||
				m.axis[a].seg[0].p.device.kind == dspace::axis_mode::rational)) {

				// a hinted grid of this very device is kept while it holds all but 0.1% of the positions
				for (const auto& h : state.hints) {

					// the same device, whatever play rate its replay had
					auto hinted_device{ setup::device(h, a) };

					hinted_device.game.rate = m.axis[a].seg[0].p.game.rate;

					if (grid[a].on || hinted_device != m.axis[a].seg[0].p)
						continue;

					const _native_grid hinted{ h.grid_step[a] > 0. ? grid_of_step(p.st[a], h.grid_step[a]) : _native_grid{} };

					if (!hinted.on)
						continue;

					auto st{ grid_streams(m.axis[a].seg[0].p, p.st[a], vals[a], state.exact_text(), hinted) };

					// a finer grid than the tablet's holds the positions too, at a higher cost: on the tablet's own grid
					// neighbouring counts are the most common gap between the counts a replay visits
					std::vector<i64> js{ st.K };

					std::sort(js.begin(), js.end());
					js.erase(std::unique(js.begin(), js.end()), js.end());

					size_t neighbours{};

					for (size_t k{ 1 }; k < js.size(); ++k)
						neighbours += js[k] - js[k - 1] == 1;

					if (neighbours * 2 < js.size())
						continue;

					if ((st.escapes.size() - std::min(st.escapes.size(), p.st[a].escapes.size())) * 1000 <= st.K.size()) {

						grid[a] = hinted;
						on_grid[a] = std::move(st);

					}

				}

				if (grid[a].on)
					continue;

				grid[a] = fit_grid(p.st[a].K);

				if (!grid[a].on)
					continue;

				on_grid[a] = grid_streams(m.axis[a].seg[0].p, p.st[a], vals[a], state.exact_text(), grid[a]);

				// a driver spreads a whole number of tablet counts over its output range (65535, a doubled 32767, 65536),
				// so a fitted step near such a one is that one. The integers of a lattice of N counts are on one of
				// range - N too, and the fit can land on either: the tablet's own is the one its counts move smoothly on.
				const double step{ (double)grid[a].A / 4294967296. };

				size_t cost{ grid_cost(m.axis[a].seg[0].p, grid[a], on_grid[a], dt, a, state.exact_text()) };

				for (const double range : { 65535., 65534., 65536. }) {

					const double n{ std::round(range / step) };

					if (std::abs(range / step - n) >= 0.05 || n < 64.)
						continue;

					for (const double counts : { n, range - n }) {

						const _native_grid exact{ counts >= 64. ? grid_of_step(p.st[a], range / counts) : _native_grid{} };

						if (!exact.on || exact.A == grid[a].A)
							continue;

						auto st{ grid_streams(m.axis[a].seg[0].p, p.st[a], vals[a], state.exact_text(), exact) };

						if (const size_t c{ grid_cost(m.axis[a].seg[0].p, exact, st, dt, a, state.exact_text()) }; c < cost) {

							cost = c;
							grid[a] = exact;
							on_grid[a] = std::move(st);

						}

					}

					break;
				}

			}

		const auto build = [&](bool use_grid) {

			bit_stream head{ model_head };

			for (int a{}; a < 2; ++a)
				write_grid(use_grid ? grid[a] : _native_grid{}, head);

			head.push_back(shared.std_default_pos);

			if (shared.std_default_pos)
				head.push_back(shared.std_default_time);

			frames::_frame_data fd{ shared.delta, shared.keys, shared.repeated };

			fd.moves = moves;

			std::vector<frames::_escape> values[2];

			for (int a{}; a < 2; ++a) {

				const bool on{ use_grid && grid[a].on };
				const dspace::axis_streams& st{ on ? on_grid[a] : p.st[a] };

				fd.K[a] = st.K;
				fd.slack[a] = st.slack;

				for (size_t j{}; j < st.escapes.size(); ++j) {

					const u32 i{ st.escapes[j].first };

					fd.escaped[a].push_back(i);
					values[a].push_back({ st.escapes[j].second, st.escape_pred[j] });

					if (on)
						grid_near(m.axis[a].seg[0].p, grid_value(grid[a], st.K[i]), values[a].back().near);

				}

			}

			const frames::_side side{ m.axis, shared.table, shared.time_grid(), state.mode(), ap, shared.std_default_time };

			if (state.file.encode_info & ORZ_ENCODE_INFO::fastest_frames)
				add_to_u8(head.data, frames::encode<cm::fastest_bit_encoder>(state.tables.standard, fd, side, shared.delta.size(), values, state.exact_text()));
			else if (state.file.encode_info & ORZ_ENCODE_INFO::fast_frames)
				add_to_u8(head.data, frames::encode<cm::fast_bit_encoder>(state.tables.standard, fd, side, shared.delta.size(), values, state.exact_text()));
			else
				add_to_u8(head.data, frames::encode(state.tables.standard, fd, side, shared.delta.size(), values, state.exact_text()));

			return head.data;
		};

		p.fitted[0] = grid[0];
		p.fitted[1] = grid[1];

		bool use_grid{ grid[0].on || grid[1].on };

		for (int a{}; a < 2; ++a)
			use_grid &= !grid[a].on || on_grid[a].escapes.size() * 20 < on_grid[a].K.size();

		auto best{ build(use_grid) };

		if (!use_grid && (grid[0].on || grid[1].on))
			if (auto g{ build(true) }; g.size() < best.size()) {

				best = std::move(g);
				use_grid = true;

			}

		if (use_grid)
			for (int a{}; a < 2; ++a)
				if (grid[a].on) {

					p.st[a].decoded = on_grid[a].decoded;
					p.grid[a] = grid[a];

				}

		p.frames = std::move(best);

		return p;
	}

	inline void write_orz(const _osr& r, const _positions& p, const _shared_streams& shared, orz_state& state) {

		u32 crc{ ORZ_CRC_SEED };

		for (size_t i{}; i < r.size(); ++i) {

			const u32 s{ shared.sample[i] };

			const bool is_default{ shared.std_default_pos && i < 2 };

			crc = crc32c_frame(crc, { r[i].delta, is_default ? POS_DEFAULT_X : p.st[0].decoded[s], is_default ? POS_DEFAULT_Y : p.st[1].decoded[s], r[i].keys });

		}

		if (state.seed.present)
			crc = crc32c_frame(crc, { -12345, 0.f, 0.f, state.seed.value });

		if (p.autopilot)
			state.file.encode_info = set_e_mode(state.file.encode_info, e_mode::osu_autopilot);

		add_container(state.file, orz_tag::frames, p.frames);

		finish_orz_file((u32)r.size(), crc, state);

	}

	// axis_mode::none marks what detection could not fit
	inline dspace::_model_info text_filled(dspace::_model_info m) {

		for (auto& ax : m.axis) {

			for (auto& sg : ax.seg) {

				if (sg.p.device.kind == dspace::axis_mode::none) {

					sg.p = {};
					sg.p.device.kind = dspace::axis_mode::text;

				}

			}

		}

		return m;
	}

	inline dspace::_model_info all_text() {

		dspace::axis_params p{};

		p.device.kind = dspace::axis_mode::text;

		dspace::_model_info m{};

		m.axis[0] = dspace::axis_model{ p };
		m.axis[1] = dspace::axis_model{ p };

		return m;
	}

	// the replay's frames with autopilot moves in place of a device model
	inline _positions autopilot_streams(std::span<const autopilot::sample> s, const autopilot::_params& ap, const std::vector<float> vals[2],
		const std::vector<int>& dt, const _shared_streams& shared, orz_state& state) {

		size_t covered{};

		const auto moves{ autopilot::fitter{ s, ap }.fit(covered) };

		return position_streams(all_text(), vals, dt, shared, state, ap, moves);
	}

	// mouse axes differ, try each other and keep the winners
	inline void try_other_mouse(orz_state& state, const std::vector<float> vals[2], const std::vector<int>& dt,
		dspace::axis_params& px, dspace::axis_params& py, dspace::axis_streams ws[2]) {

		const bool differ{
			px.game.scale != py.game.scale || px.game.unrounded_sub != py.game.unrounded_sub || px.device.mouse.sens != py.device.mouse.sens || px.device.mouse.poll != py.device.mouse.poll ||
			px.game.rate != py.game.rate
		};

		if (px.device.kind != dspace::axis_mode::mouse || py.device.kind != dspace::axis_mode::mouse || !differ)
			return;

		const dspace::axis_params from[2]{ py, px };
		dspace::axis_params* to[2]{ &px, &py };

		for (int a{}; a < 2; ++a) {

			size_t best{ axis_cost(ws[a], dt, a, state.exact_text()) };

			for (const auto& [W, H] : PM_RES) {

				const auto [ratio, ox, oy] { dspace::playfield(W, H) };

				if (ratio != from[a].game.scale)
					continue;

				auto q{ from[a] };

				q.game.off = a ? oy : ox;

				auto st{ dspace::encode_axis(q, vals[a], dt, state.exact_text(), {}, vals[a].size() / 2) };

				if (st.escapes.size() * 2 > vals[a].size())
					continue;

				if (const size_t c{ axis_cost(st, dt, a, state.exact_text()) }; c < best) {

					best = c;
					*to[a] = q;
					ws[a] = std::move(st);

				}

			}

		}

	}

	// x's device on y (the same window, y's offset), taken when it misses at most 0.1% of y's samples
	inline bool device_from_x(orz_state& state, const std::vector<float>& vals, const std::vector<int>& dt, const dspace::axis_params& px,
		dspace::axis_params& py, dspace::axis_streams& streams) {

		std::vector<dspace::axis_params> cands;

		if (px.device.kind == dspace::axis_mode::rational) {

			// the same lattice, the offset from the most common residue
			const i64 D{ px.device.rational.D }, A{ px.device.rational.a };

			std::map<i64, size_t> res;

			for (size_t i{ 2 }; i < vals.size(); i += std::max<size_t>(1, vals.size() / 2000))
				++res[((std::llround((double)vals[i] * D) % A) + A) % A];

			i64 B{};
			size_t bc{};

			for (const auto& [v, n] : res) {

				if (n > bc) {

					bc = n;
					B = v;

				}

			}

			auto q{ px };

			q.device.rational.b = (int)B;

			cands.push_back(q);

		} else if (px.device.kind == dspace::axis_mode::mouse ||
			(px.device.kind == dspace::axis_mode::absolute && px.device.absolute.mapping == dspace::tablet_mapping::window)) {

			for (const auto& [W, H] : PM_RES) {

				const auto [ratio, ox, oy] { dspace::playfield(W, H) };

				if (ratio != px.game.scale)
					continue;

				auto q{ px };

				q.game.off = oy;

				if (q.device.kind == dspace::axis_mode::absolute) {

					q.game.window_half = (float)(H / 2);
					q.device.absolute.span = (float)H;

				}

				cands.push_back(q);

			}

		}

		const size_t limit{ vals.size() / 1000 };

		for (const auto& q : cands) {

			auto st{ dspace::encode_axis(q, vals, dt, state.exact_text(), {}, limit + 1) };

			if (st.escapes.size() + st.repeats <= limit) {

				py = q;
				streams = std::move(st);

				return true;
			}

		}

		return false;
	}

	inline void share_mouse(orz_state& state, const std::vector<float> vals[2], const std::vector<int>& dt,
		dspace::axis_params& px, dspace::axis_params& py, dspace::axis_streams ws[2]) {

		dspace::axis_params* p[2]{ &px, &py };

		for (int m{}; m < 2; ++m) {

			const int a{ 1 - m };

			if (p[m]->device.kind != dspace::axis_mode::mouse || p[a]->device.kind == dspace::axis_mode::mouse || p[a]->device.kind == dspace::axis_mode::none)
				continue;

			size_t best{ axis_cost(ws[a], dt, a, state.exact_text()) };

			for (const auto& [W, H] : PM_RES) {

				const auto [ratio, ox, oy] { dspace::playfield(W, H) };

				if (ratio != p[m]->game.scale)
					continue;

				auto q{ *p[m] };

				q.game.off = a ? oy : ox;

				auto st{ dspace::encode_axis(q, vals[a], dt, state.exact_text(), {}, vals[a].size() / 2) };

				if (st.escapes.size() * 2 > vals[a].size())
					continue;

				if (const size_t c{ axis_cost(st, dt, a, state.exact_text()) }; c < best) {

					best = c;
					*p[a] = q;
					ws[a] = std::move(st);

				}

			}

		}

	}

	inline bool detect_device_model(orz_state& state, const std::vector<float> vals[2], const std::vector<int>& dt,
		const _shared_streams& shared, dspace::_model_info& info, _positions& best) {

		// whole replay, one model per axis
		dspace::_model_info whole{};

		dspace::axis_streams ws[2];

		{

			dspace::axis_params px, py;

			px = detect_axis_model(state, vals[0], dt, 0, nullptr, ws[0]);

			// y's own search only when x's device does not fit it (not osu!catch, whose y is the catcher's fixed height)
			if (px.device.kind != dspace::axis_mode::none) {

				if (game_mode(state.mode()) == 2 || !device_from_x(state, vals[1], dt, px, py, ws[1]))
					py = detect_axis_model(state, vals[1], dt, 1, &px, ws[1]);

			} else {

				// x found nothing: detect y on its own, then retry x with y's mapping
				py = detect_axis_model(state, vals[1], dt, 1, nullptr, ws[1]);

				if (py.device.kind == dspace::axis_mode::absolute)
					px = detect_axis_model(state, vals[0], dt, 0, &py, ws[0]);

			}

			if (px.device.kind != dspace::axis_mode::none || py.device.kind != dspace::axis_mode::none)
				refine_window(state, vals, dt, px, py, ws);

			try_other_mouse(state, vals, dt, px, py, ws);
			share_mouse(state, vals, dt, px, py, ws);

			whole.axis[0] = dspace::axis_model{ px };
			whole.axis[1] = dspace::axis_model{ py };

		}

		const bool whole_any{ !whole.axis[0].all_none() || !whole.axis[1].all_none() };
		const bool whole_ok{ !whole.axis[0].any_none() && !whole.axis[1].any_none() };

		bool try_segments{ !whole_ok };

		for (int a{}; a < 2 && whole_ok; ++a)
			try_segments |= (ws[a].escapes.size() + ws[a].repeats) * 100 > vals[a].size() * 3;

		// shortest round-trip text (osu!lazer) dont gain from segments
		// lazer will need to be handled properly in the future if we want higher gain
		if (state.exact_text())
			try_segments = false;

		bool found{};

		if (whole_any) {

			info = text_filled(whole);
			best = position_streams(info, vals, dt, shared, state);
			found = true;

		}

		if (try_segments) {

			dspace::_model_info seg{};

			const auto desktops = [&](int a) {

				return whole.axis[a].seg[0].p.device.kind != dspace::axis_mode::mouse;
			};

			seg.axis[0] = segment_axis(state, vals[0], dt, 0, nullptr, whole.axis[0].all_none(), desktops(0));
			seg.axis[1] = segment_axis(state, vals[1], dt, 1, &seg.axis[0], whole.axis[1].all_none(), desktops(1));

			if (!seg.axis[0].all_none() || !seg.axis[1].all_none()) {

				const auto filled{ text_filled(seg) };

				auto p{ position_streams(filled, vals, dt, shared, state) };

				if (!found || p.size() < best.size()) {

					info = filled;
					best = std::move(p);
					found = true;

				}

			}

		}

		return found;
	}

	inline void encode(const _osr& original, orz_state& state) {

		const bool std_default_pos{ std_default_pos_of(original) };
		const bool std_default_time{ std_default_pos && original[0].delta == 0 && original[1].delta == -1 };

		_osr moved;

		if (std_default_pos) {

			moved = original;

			for (int k{}; k < 2; ++k) {

				moved[k].x = moved[2].x;
				moved[k].y = moved[2].y;

			}

		}

		const _osr& r{ std_default_pos ? moved : original };

		const _shared_streams shared{ shared_streams(r, std_default_pos, std_default_time) };

		// the .osr's mods say the play rate; bare replay data leaves it to the device search (report_rates)
		state.play_rate = state.header_rate > 0.f ? state.header_rate : 1.f;

		const bool relax{ is_relax(state.mode()) };

		const std::vector<int> dt{ repeat::sample_dt(shared.delta, shared.repeated) };

		std::vector<float> vals[2];

		for (size_t i{}; i < r.size(); ++i) {

			if (shared.repeated[i])
				continue;

			vals[0].push_back(r[i].x);
			vals[1].push_back(r[i].y);

		}

		dspace::_model_info info{ all_text() };
		_positions best{};

		// autopilot: clearly autopilot replays skip the device models, others that have none try it against text. Relax
		// and autopilot do not go together.
		const auto ap_samples{ autopilot_samples(r, shared, state.exact_text()) };

		if (const auto ap{ relax ? autopilot::_params{} : autopilot::detect(ap_samples, AUTOPILOT_WINDOWS, std::size(AUTOPILOT_WINDOWS)) }; ap.on)
			best = autopilot_streams(ap_samples, ap, vals, dt, shared, state);
		else {

			// a forced hint's device on both axes is the model, with no search
			bool forced{};

			for (const auto& h : state.hints) {

				if (!h.force)
					continue;

				dspace::axis_params q[2]{ setup::device(h, 0), setup::device(h, 1) };

				if (q[0].device.kind == dspace::axis_mode::none || q[1].device.kind == dspace::axis_mode::none)
					continue;

				for (int a{}; a < 2; ++a) {

					q[a].game.rate = state.play_rate;
					info.axis[a] = dspace::axis_model{ q[a] };

				}

				best = position_streams(info, vals, dt, shared, state);
				forced = true;

				break;
			}

			const bool has_model{ forced || (!state.debug.disable_device && dt.size() >= 64 && detect_device_model(state, vals, dt, shared, info, best)) };

			// both axes as text, only tried when the model misses 5% or more of an axis's frames
			const auto misses = [&](int a) {

				return (best.st[a].escapes.size() + best.st[a].repeats) * 20 >= dt.size();
			};

			if (!has_model || (!forced && (misses(0) || misses(1)))) {

				auto text{ position_streams(all_text(), vals, dt, shared, state) };

				if (!has_model || text.size() <= best.size()) {

					info = all_text();
					best = std::move(text);

				}

			}

			const auto text_only = [&](int a) {

				return info.axis[a].seg.size() == 1 && info.axis[a].seg[0].p.device.kind == dspace::axis_mode::text;
			};

			if (text_only(0) && text_only(1)) {

				if (const auto search{ relax ? autopilot::_params{} : search_autopilot(ap_samples) }; search.on) {

					auto moves{ autopilot_streams(ap_samples, search, vals, dt, shared, state) };

					if (moves.size() < best.size())
						best = std::move(moves);

				}

			}

		}

		float missed[2];

		for (int a{}; a < 2; ++a)
			missed[a] = vals[a].empty() ? 0.f : (float)((double)best.st[a].escapes.size() / (double)vals[a].size());

		state.setup = setup::of(info, best.fitted, missed);
		state.clock_rate = frame_time::clock_rate(r, state.header_rate, (double)shared.period().num / (double)shared.period().den);

		write_orz(r, best, shared, state);

		++state.stats.files;

		state.stats.frames += r.size();

		for (int a{}; a < 2; ++a) {

			for (const auto& sg : info.axis[a].seg)
				++state.stats.axis[(size_t)sg.p.device.kind];

			state.stats.escapes += best.st[a].escapes.size();

		}

		if (info.axis[0].seg.size() > 1 || info.axis[1].seg.size() > 1)
			++state.stats.segmented;

	}

	struct _frames_head {
		std::vector<int> table;
		frame_time::_time_grid grid{};
		dspace::_model_info info;
		autopilot::_params ap;
		_native_grid grid_of[2]{};
		bool std_default_pos{}, std_default_time{};
		std::span<const u8> rest;
	};

	inline bool read_head(std::span<const u8> data, bool autopilot, _frames_head& h) {

		_bit_view head{ data };

		if (!frame_time::read_table(head, h.table, h.grid) || !dspace::read_info(head, h.info) || !read_autopilot(head, h.ap, autopilot))
			return false;

		for (int a{}; a < 2; ++a)
			if (!read_grid(head, h.grid_of[a]))
				return false;

		h.std_default_pos = head.read_bits<1>() != 0;
		h.std_default_time = h.std_default_pos && head.read_bits<1>() != 0;

		h.rest = head.get_tail();

		return true;
	}

	template<typename Reader>
	inline bool decode_profile(u32 encode_info, std::span<const u8> data, std::vector<_osr_frame>& R, _table_memory& tables) {

		_frames_head head{};

		if (!read_head(data, get_e_mode(encode_info) == e_mode::osu_autopilot, head))
			return R.empty();

		const dspace::_model_info& info{ head.info };
		const auto& grid_of{ head.grid_of };

		frames::_side side{};

		side.axis = info.axis;
		side.table = std::move(head.table);
		side.grid = head.grid;
		side.mode = get_e_mode(encode_info);
		side.ap = head.ap;
		side.std_default_time = head.std_default_time;

		frames::_frame_data fd{};
		Reader reader{ tables.standard, head.rest, fd, side, R.size() };

		const auto sample{ repeat::sample_of(fd.repeated) };

		// tablet grid indices back to values
		for (int a{}; a < 2; ++a)
			if (grid_of[a].on)
				for (auto& k : fd.K[a])
					k = grid_value(grid_of[a], k);

		const bool exact_text{ (encode_info & ORZ_ENCODE_INFO::text_shortest) != 0 };

		std::vector<float> out;

		for (int a{}; a < 2; ++a) {

			size_t n{};

			const auto escape_value = [&](float pred) {

				frames::_escape v{ 0.f, pred };

				// a damaged file can ask for more escapes than it has
				if (grid_of[a].on)
					grid_near(info.axis[a].seg[0].p, fd.K[a].at(fd.escaped[a].at(n++)), v.near);

				return reader.escape(v, a, exact_text);
			};

			if (!dspace::decode_axis(info.axis[a], fd.K[a], fd.slack[a], fd.escaped[a], fd.dt, exact_text, out, escape_value))
				return false;

			for (size_t i{}; i < R.size(); ++i)
				(&R[i].x)[a] = out[sample[i]];

		}

		for (size_t i{}; i < R.size(); ++i) {

			R[i].delta = fd.delta[i];
			R[i].keys = fold_keys(fd.keys[i]);

		}

		for (size_t i{}; head.std_default_pos && i < std::min<size_t>(2, R.size()); ++i) {

			R[i].x = POS_DEFAULT_X;
			R[i].y = POS_DEFAULT_Y;

		}

		return true;
	}

	inline bool decode(u32 encode_info, std::span<const u8> data, std::vector<_osr_frame>& R, _table_memory& tables) {
		if (encode_info & ORZ_ENCODE_INFO::fastest_frames)
			return decode_profile<frames::_fastest_reader>(encode_info, data, R, tables);

		return (encode_info & ORZ_ENCODE_INFO::fast_frames) ?
			decode_profile<frames::_fast_reader>(encode_info, data, R, tables) :
			decode_profile<frames::_reader>(encode_info, data, R, tables);
	}

}

}
