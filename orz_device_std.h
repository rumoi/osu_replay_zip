#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <numeric>
#include <set>
#include <span>
#include <unordered_map>
#include <vector>

#include "orz_state.h"
#include "device_space.h"

namespace orz {

namespace standard {

	// least squares on the frames within a shrinking window of the lattice
	static void refine_tight(const std::span<const float> p, double& s, double& c) {

		std::vector<std::pair<double, double>> in; // (k, x)

		in.reserve(p.size());

		for (double w : { 0.05, 0.01, 0.002, 0.0005, 0.0002, 0.0001, 0.0001 }) {

			const double win{ std::min(w, 0.2 * s) };

			in.clear();

			double mk{}, mx{};

			for (double x : p) {

				const double k{ (double)dspace::round_i64((x - c) / s) };

				if (std::abs(x - (k * s + c)) > win)
					continue;

				in.push_back({ k, x });

				mk += k;
				mx += x;

			}

			if (in.size() < 100)
				return;

			mk /= in.size();
			mx /= in.size();

			double sxx{}, sxy{};

			for (auto& [k, x] : in) {

				sxx += (k - mk) * (k - mk);
				sxy += (k - mk) * (x - mx);

			}

			if (sxx <= 0.0)
				return;

			s = sxy / sxx;
			c = mx - s * mk;

		}

	}

	static bool no_larger_lattice(const std::vector<double>& t, double s) {

		constexpr double thr{ 0.9 };

		const double tol{ std::min(std::max(2e-4, 3e-3 * s), 0.15 * s) };
		const double inv{ 1.0 / s };

		std::vector<int> ks;

		ks.reserve(t.size());

		size_t loose{}, ones{};

		for (double v : t) {

			const int k{ (int)(v * inv + 0.5) };

			if (k >= 1 && std::abs(v - k * s) < tol) {

				ks.push_back(k);
				ones += k == 1;

			} else
				++loose;

		}

		const double n{ (double)t.size() };

		if ((double)(t.size() - ones) / n < thr)
			return true;

		std::sort(ks.begin(), ks.end());

		std::vector<std::pair<int, size_t>> pw;

		for (size_t i{}; i < ks.size();) {

			size_t j{ i };

			while (j < ks.size() && ks[j] == ks[i])
				++j;

			int k{ ks[i] };

			const size_t w{ j - i };

			const auto add = [&](int q) {

				for (auto& [p, c] : pw) {

					if (p == q) {

						c += w;

						return;
					}

				}

				pw.push_back({ q, w });

			};

			for (int q{ 2 }; q * q <= k; ++q) {

				if (k % q)
					continue;

				add(q);

				while (k % q == 0)
					k /= q;

			}

			if (k > 1)
				add(k);

			i = j;

		}

		for (auto& [q, w] : pw)
			if ((double)(w + loose) / n >= thr)
				return false;

		return true;
	}

	static bool fit_axis_step(const std::span<const float> p, double& step, double& offset) {

		constexpr double TAU{ 6.283185307179586 };

		std::vector<double> d;

		d.reserve(p.size());

		for (size_t i{ 1 }; i < p.size(); ++i) {

			const double v{ std::abs((double)p[i] - p[i - 1]) };

			if (v > 0.002 && v < 300.0)
				d.push_back(v);

		}

		if (d.size() < 100)
			return false;

		std::sort(d.begin(), d.end());

		const size_t st{ std::max<size_t>(1, d.size() / 3000) };

		std::vector<double> t;

		t.reserve((d.size() + st - 1) / st);

		for (size_t i{}; i < d.size(); i += st)
			t.push_back(d[i]);

		std::unordered_map<double, double> climbed[9];

		const auto climb = [&](double s) {

			double path[9];
			int pass{};

			size_t last_end{ SIZE_MAX };
			double last_s{};

			for (double vmax{ 1.0 }; vmax <= 256.0; vmax *= 2.0, ++pass) {

				const size_t end{ (size_t)(std::upper_bound(t.begin(), t.end(), vmax) - t.begin()) };

				// an unchanged step over the same prefix is already a fixed point
				if (end != last_end || s != last_s) {

					last_end = end;
					last_s = s;

					const double inv{ 1.0 / s }, tol{ 0.2 * s };

					double a{}, b{};

					for (size_t j{}; j < end; ++j) {

						const double v{ t[j] };
						const double k{ (double)(int)(v * inv + 0.5) };

						if (k >= 1 && std::abs(v - k * s) < tol) {

							a += v * k;
							b += k * k;

						}

					}

					if (b > 0)
						s = a / b;

				}

				if (const auto it{ climbed[pass].find(s) }; it != climbed[pass].end()) {

					s = it->second;

					break;
				}

				path[pass] = s;

			}

			for (int q{}; q < pass; ++q)
				climbed[q].emplace(path[q], s);

			return s;
		};

		const auto fit = [&](double s) {

			const double tol{ std::min(std::max(2e-4, 3e-3 * s), 0.15 * s) };
			const double inv{ 1.0 / s };

			size_t ok{}, miss{};

			for (double v : t) {

				if (std::abs(v - (double)(int)(v * inv + 0.5) * s) < tol)
					++ok;
				else if (double(t.size() - ++miss) / t.size() < 0.97)
					return 0.0;

			}

			return double(ok) / t.size();
		};

		const auto search = [&](int max_div, double min_step) {

			double best{}, last{ -1.0 };
			int tried{};

			for (size_t j{}; j < d.size() && tried < 8; ++j) {

				if (d[j] - last < 0.002)
					continue;

				last = d[j];
				++tried;

				for (int m{ 1 }; m <= max_div && d[j] / m >= min_step; ++m) {

					const double s{ climb(d[j] / m) };

					if (s < min_step)
						continue;

					if (s > best && fit(s) >= 0.97) {

						best = s;

						if (s >= 0.004 && no_larger_lattice(t, s))
							return best;

					}

				}

			}

			return best;
		};

		double best_s{ search(32, 0.004) };

		if (best_s == 0.0)
			best_s = search(256, 3e-4);

		if (best_s == 0.0)
			return false;

		double s{ best_s }, c{};

		{

			double cs{}, sn{};

			for (double x : p) {

				const double a{ TAU * x / s };

				cs += std::cos(a);
				sn += std::sin(a);

			}

			c = std::atan2(sn, cs) / TAU * s;

		}

		refine_tight(p, s, c);

		step = s;
		offset = c;

		return true;
	}

	static void neighbour_steps(const std::span<const float> p, double s, std::vector<std::pair<double, double>>& steps) {

		std::vector<double> d; d.reserve(p.size());

		for (size_t i{ 1 }; i < p.size(); ++i)
			if (const double v{ std::abs((double)p[i] - p[i - 1]) }; v > 0.5 * s && v < 300.)
				d.push_back(v);

		int k{};

		if (d.size() >= 100) {

			const auto at{ d.begin() + d.size() / 10 };

			std::nth_element(d.begin(), at, d.end());

			k = (int)(*at / s + 0.5);

		}

		for (const int j : { 1, -1 }) {

			if (k < 4 || k > 1000)
				break;

			constexpr double TAU{ 6.283185307179586 };

			double q{ s * k / (k + j) }, cs{}, sn{};

			for (const double x : p) {

				cs += std::cos(TAU * x / q);
				sn += std::sin(TAU * x / q);

			}

			double qc{ std::atan2(sn, cs) / TAU * q };

			refine_tight(p, q, qc);

			steps.push_back({ q, qc });

		}

	}

	// rough coded size of an axis in bits, for comparing models: Elias gamma lengths of the move predictor's residuals,
	// the slack, the gaps between escapes and their values
	static size_t axis_cost(const dspace::axis_streams& a, std::span<const int> dt, int axis, bool exact_text) {

		const auto gamma = [](u64 v) { return 1 + 2 * (size_t)std::bit_width(v); };

		size_t bits{};

		dspace::move_predictor mp{};

		for (size_t i{}; i < a.K.size(); ++i) {

			const i64 d{ a.K[i] - (i ? a.K[i - 1] : 0) };
			const i64 pr{ mp.pred(axis, dt[i]) };

			bits += gamma((u64)std::abs(d - pr));

			mp.push(axis, d);
			mp.advance(dt[i]);

		}

		for (const int k : a.slack)
			bits += k ? gamma((u64)std::abs(rc::unzz((u64)k))) + 1 : 0;

		u32 prev{};

		for (size_t j{}; j < a.escapes.size(); ++j) {

			i64 diff{};

			const bool near{ dspace::escape_diff(a.escapes[j].second, a.escape_pred[j], exact_text, diff) };

			bits += gamma(a.escapes[j].first - prev) + (near ? gamma(rc::zz(diff)) : 33);
			prev = a.escapes[j].first;

		}

		return bits;
	}

	// axis_cost for comparing devices: fully coded, cursor replays come out smaller on tablets and larger on mice than the
	// estimate says (not relax osu!catch)
	static size_t device_cost(const orz_state& state, const dspace::axis_params& p, const dspace::axis_streams& a, std::span<const int> dt,
		int axis) {

		const size_t bits{ axis_cost(a, dt, axis, state.exact_text()) };

		if (game_mode(state.mode()) == 2)
			return bits;

		return p.mode == dspace::axis_mode::absolute ? bits * 15 / 16 : p.mode == dspace::axis_mode::mouse ? bits * 17 / 16 : bits;
	}

	using dspace::PM_RES;

	struct prefix_result {
		double score;
		size_t escapes;
	};

	static size_t escape_cap(double bound) {
		return bound < 40.0 * 1e15 ? (size_t)(bound / 40.0) : SIZE_MAX;
	}

	static prefix_result prefix_eval(const orz_state& state, const dspace::axis_params& p, std::span<const float> vals,
		std::span<const int> dt, std::span<const dotnet::text_bounds> bounds, size_t n, size_t max_escapes = SIZE_MAX) {

		n = std::min(n, vals.size());

		const auto o{ dspace::encode_axis(p, vals.first(n), dt.first(n), state.exact_text(), bounds.first(n), max_escapes) };

		if (o.escapes.size() > max_escapes)
			return { INFINITY, o.escapes.size() };

		double score{ 40.0 * o.escapes.size() };

		for (const int k : o.slack)
			score += k == 0 ? 0.3 : 2.0 + 2.0 * std::log2(1.0 + std::abs(rc::unzz((u64)k)));

		return { score, o.escapes.size() };
	}

	// floats just below a power of ten hold the most information
	static std::vector<size_t> informative_frames(const orz_state& state, std::span<const float> vals) {

		std::vector<size_t> pick;

		for (size_t i{ 2 }; i < vals.size(); ++i) {

			const float a{ std::abs(vals[i]) };

			if (!std::isfinite(a) || a == 0.f)
				continue;

			if (!state.exact_text()) {

				const double spacing{ (double)dspace::next_up(a) - a };

				
				static const auto P10 = [] {

					std::array<double, 100> t{};

					for (int e{ -60 }; e < 40; ++e)
						t[e + 60] = std::pow(10.0, e);

					return t;
				}();

				const size_t d{ (size_t)(std::upper_bound(P10.begin(), P10.end(), (double)a) - P10.begin()) - 1 };
				const double printed{ P10[d - 6] };

				if (printed / spacing > 2.2)
					continue;

			}

			pick.push_back(i);

		}

		if (pick.size() < 32)
			return {};

		return pick;
	}

	static std::vector<size_t> spread_frames(const std::vector<size_t>& pick, size_t n) {

		if (pick.size() <= n)
			return pick;

		std::vector<size_t> thin(n);

		for (size_t k{}; k < n; ++k)
			thin[k] = pick[k * pick.size() / n];

		return thin;
	}

	// the score if below bound, else infinity
	static double prefix_score(const orz_state& state, const dspace::axis_params& p, std::span<const float> vals,
		std::span<const int> dt, std::span<const dotnet::text_bounds> bounds, size_t n, double bound = INFINITY) {
		return prefix_eval(state, p, vals, dt, bounds, n, escape_cap(bound)).score;
	}

	static bool near_grid(double v, double step, double tol) {

		const double q{ v / step };

		return std::abs(q - (double)dspace::round_i64(q)) <= tol;
	}

	static int setting_values(double v, double tol, bool exact, float out[2]) {

		int n{};

		if (near_grid(v, 0.01, tol))
			out[n++] = (float)(std::round(v * 100.) / 100.);

		if (exact)
			out[n++] = (float)v;

		return n;
	}
	
	struct virtual_group {
		size_t from, to;
		float ratio, off;
		int span, o_from, o_to;
	};

	struct lb_frame {
		float flo, fhi;
		bool counted;
	};

	static std::vector<lb_frame> lb_frames(std::span<const float> v, std::span<const dotnet::text_bounds> b) {

		std::vector<lb_frame> out(v.size());

		for (size_t j{}; j < v.size(); ++j) {

			auto& f{ out[j] };

			f.counted = false;

			if (!std::isfinite(v[j]) || !b[j].contains(v[j]) || (j && b[j].contains(v[j - 1])))
				continue;

			float lo{ v[j] }, hi{ v[j] };
			int n{};

			for (; n < 64 && b[j].contains(dspace::next_down(lo)); ++n)
				lo = dspace::next_down(lo);

			for (; n < 64 && b[j].contains(dspace::next_up(hi)); ++n)
				hi = dspace::next_up(hi);

			if (n < 64)
				f = { lo, hi, true };

		}

		return out;
	}

	static double float_spacing(double a) {

		const float f{ (float)std::abs(a) };

		return (double)dspace::next_up(f) - (double)f;
	}

	static int virtual_lb(const std::vector<lb_frame>& fr, const virtual_group& G, std::vector<u16>& votes) {

		const int n_o{ G.o_to - G.o_from + 1 };

		votes.assign(n_o, 0);

		std::vector<int> stamp(n_o, -1);

		const float inv{ 1.f / G.ratio }, off{ G.off }, span_f{ (float)G.span };

		const auto field = [&](float P) {

			const float d{ P - off };

			return d * inv;
		};

		const i64 span{ G.span };
		const i64 g{ std::min<i64>(span & -span, 65536) };
		const i64 M{ 65536 / g };
		const int g_shift{ std::countr_zero((u64)g) };
		const u64 sp{ (u64)(span / g) }; // odd

		u64 sinv{ 1 };

		for (int it{}; it < 6; ++it)
			sinv *= 2 - sp * sinv;

		int counted{};

		for (size_t j{}; j < fr.size(); ++j) {

			const auto& F{ fr[j] };

			if (!F.counted)
				continue;

			float plo{ (float)((double)F.flo * G.ratio + off) };
			float phi{ (float)((double)F.fhi * G.ratio + off) };

			int n1{}, n2{}, n3{}, n4{};

			for (; n1 < 64 && field(plo) >= F.flo; ++n1)
				plo = dspace::next_down(plo);

			for (; n2 < 64 && field(plo) < F.flo; ++n2)
				plo = dspace::next_up(plo);

			for (; n3 < 64 && field(phi) <= F.fhi; ++n3)
				phi = dspace::next_up(phi);

			for (; n4 < 64 && field(phi) > F.fhi; ++n4)
				phi = dspace::next_down(phi);

			if (n1 == 64 || n2 == 64 || n3 == 64 || n4 == 64 || !std::isfinite(plo) || !std::isfinite(phi))
				continue;

			++counted;

			if (plo > phi)
				continue; // every origin misses it

			const double A{
				1.01 * std::max({ std::abs((double)plo - G.o_to), std::abs((double)plo - G.o_from),
					std::abs((double)phi - G.o_to), std::abs((double)phi - G.o_from) }) + 2.0
			};

			const double e{
				0.5 * float_spacing(A) + 0.5 * float_spacing(2.0 * std::max(std::abs((double)plo), std::abs((double)phi)))
			};

			const i64 lo_t{ (i64)std::floor(((double)plo - e) * 65536.0) - 1 };
			const i64 hi_t{ (i64)std::ceil(((double)phi + e) * 65536.0) + 1 };

			if (hi_t - lo_t >= 65536) {

				--counted; // accepts every residue

				continue;
			}

			for (i64 t{ lo_t + ((g - (lo_t & (g - 1))) & (g - 1)) }; t <= hi_t; t += g) {

				const i64 L1{ (i64)((((u64)(t >> g_shift)) * sinv) & (u64)(M - 1)) };
				const i64 Lmin{ (i64)std::ceil((double)(t - 65536ll * G.o_to) / (double)span) };
				const i64 Lmax{ (i64)std::floor((double)(t - 65536ll * G.o_from) / (double)span) };

				for (i64 L{ Lmin + ((L1 - Lmin) & (M - 1)) }; L <= Lmax; L += M) {

					const i64 o{ (t - L * span) >> 16 }; // exact

					// absolute_window_pos, desktop mapping at speed 1
					float a{ (float)L / 65536.f };

					a = a * span_f;
					a = a - (-(float)o);

					if (a >= plo && a <= phi && stamp[o - G.o_from] != (int)j) {

						stamp[o - G.o_from] = (int)j;

						++votes[o - G.o_from];

					}

				}

			}

		}

		return counted;
	}

	// tries to find the best model for one axis.
	// failing to find one is the huge perf problem - having to try and bruteforce an acceptable solution
	static dspace::axis_params detect_axis_model(orz_state& state, std::span<const float> vals, std::span<const int> dt,
		int axis, const dspace::axis_params* other, dspace::axis_streams& streams, bool virtual_desktops = true, bool refine = false, double fit_s = 0., double fit_c = 0.) {

		const dspace::axis_params none{};

		streams = {};

		if (vals.size() < 64)
			return none;

		std::vector<dotnet::text_bounds> bounds(vals.size());

		for (size_t i{}; i < vals.size(); ++i)
			bounds[i] = dotnet::text_bounds{ vals[i], state.exact_text() };

		const std::span<const float> p{ vals.subspan(2) };

		// no step may be found here even though the other axis found a mapping
		const bool reuse_absolute{ other && other->mode == dspace::axis_mode::absolute };

		double s{}, c{};

		if (refine) {

			s = fit_s;
			c = fit_c;

		} else if (!fit_axis_step(p, s, c) && !reuse_absolute)
			return none;

		std::vector<std::pair<double, double>> steps{ { s, c } };
		
		if (refine)
			neighbour_steps(p, s, steps);

		std::vector<dspace::axis_params> cands;

		// check for a simple rational term first

		for (int D{ 1 }; !refine && D <= 65536; ++D) {

			const double a{ s * D };

			if (std::round(a) < 1. || std::abs(a - std::round(a)) >= 0.0001)
				continue;

			const i64 A{ std::llround(a) };

			std::map<i64, size_t> res;

			for (size_t i{}; i < p.size(); i += std::max<size_t>(1, p.size() / 2000))
				++res[((std::llround((double)p[i] * D) % A) + A) % A];

			i64 B{};
			size_t bc{};

			for (auto& [v, n] : res) {

				if (n > bc) {

					bc = n;
					B = v;

				}

			}

			dspace::axis_params q{};
			q.mode = dspace::axis_mode::rational;

			q.rational.D = D;

			q.rational.a = (int)A;
			q.rational.b = (int)B;

			cands.push_back(q);

			break;
		}

		constexpr size_t PREFIX{ 2000 };

		// refine tries a sensitivity per window, ranks them on fewer frames
		constexpr size_t REFINE_PREFIX{ 500 };

		{ // window sizes where playfield scale turns the step into a 0.01 sensitivity

			std::vector<dspace::axis_params> mouse;

			const auto add = [&](dspace::axis_params q) {

				for (const auto& m : mouse)
					if (m.field.scale == q.field.scale && m.field.off == q.field.off
						&& m.mouse.sens == q.mouse.sens)
						return;

				mouse.push_back(q);

			};

			if (other && other->mode == dspace::axis_mode::mouse) {

				// same window and mouse as the other axis, so only the offset differs

				for (const auto& [W, H] : PM_RES) {

					const auto [ratio, ox, oy] { dspace::playfield(W, H) };

					if (ratio != other->field.scale)
						continue;

					auto q{ *other };

					q.field.off = axis ? oy : ox;

					add(q);

				}

			}

			for (const auto& [s, c] : steps)
			for (const auto& [W, H] : PM_RES) {

				const auto [ratio, ox, oy] { dspace::playfield(W, H) };

				const double sens{ s * ratio };

				float sv[2];
				const int n_sens{ setting_values(sens, 0.03, refine, sv) };

				if (sens < 0.05 || sens > 10.0 || n_sens == 0)
					continue;

				dspace::axis_params q{};

				q.mode = dspace::axis_mode::mouse;
				q.field.scale = ratio;
				q.mouse.poll = 1000.f;
				q.field.off = axis ? oy : ox;

				for (int i{}; i < n_sens; ++i) {

					q.mouse.sens = sv[i];
					add(q);

				}

			}

			if (!mouse.empty()) {

				dspace::axis_params best{};

				double best_score{ std::numeric_limits<double>::max() };

				for (const auto& q : mouse) {

					const double sc{ prefix_score(state, q, vals, dt, bounds, refine ? REFINE_PREFIX : PREFIX, best_score) };

					if (sc < best_score) {

						best_score = sc;
						best = q;

					}

				}

				const auto base{ best };

				for (float poll : { 125.f, 250.f, 500.f, 1000.f, 2000.f, 4000.f, 8000.f }) {

					if (poll == base.mouse.poll)
						continue;

					auto q{ base };

					q.mouse.poll = poll;

					const double sc{ prefix_score(state, q, vals, dt, bounds, refine ? REFINE_PREFIX : PREFIX, best_score) };

					if (sc < best_score) {

						best_score = sc;
						best = q;

					}

				}

				cands.push_back(best);

			}

		}

		{ // absolute postion device, 0-65535

			// just check through some common axis sizes
			constexpr int SPANS[]{
				1920, 2560, 1366, 1360, 1600, 1280, 1680, 1440, 1024, 800,
				3840, 3440, 1536, 1152, 1400, 1080, 768, 900, 720, 1050, 1200, 600, 2160, 864, 960
			};

			std::vector<dspace::axis_params> abs;

			if (reuse_absolute) {

				for (const auto& [W, H] : PM_RES) {

					const auto [ratio, ox, oy] { dspace::playfield(W, H) };

					if (ratio != other->field.scale)
						continue;

					auto q{ *other };

					q.field.off = axis ? oy : ox;
					q.tablet.half = (float)((axis ? H : W) / 2);

					if (other->tablet.mapping == dspace::tablet_mapping::window) {

						q.tablet.span = (float)(axis ? H : W);
						abs.push_back(q);

					} else {

						for (const int span : SPANS) {

							auto r{ q };

							r.tablet.span = (float)span;
							r.tablet.origin = 0.f;

							abs.push_back(r);

						}

						const double product{ (double)other->tablet.span * other->tablet.speed };
						const double units{ s * ratio * 65536. };

						for (int h{ 5 }; h <= 1000 && s > 0.; ++h) {

							const float sp{ (float)(h / 100.) };
							const double other_span{ product / sp };

							if (std::abs(other_span - std::round(other_span)) > 1e-3)
								continue;

							const double span_d{ units / sp };
							const int span{ (int)std::lround(span_d) };

							if (span < 100 || std::abs(span_d - span) > 0.25)
								continue;

							auto r{ q };

							r.tablet.span = (float)span;
							r.tablet.speed = sp;

							const double pos_c{ c * ratio + q.field.off };
							const double base{ (pos_c - q.tablet.half) / sp + q.tablet.half };
							const double step{ (double)span / 65536. };

							int found{};

							for (int k{}; found < 4 && k <= 8000; ++k) {

								const int o{ k & 1 ? -(k + 1) / 2 : k / 2 };

								if (near_grid(base - o, step, 0.03)) {

									r.tablet.origin = (float)o;
									abs.push_back(r);
									++found;

								}

							}

						}

					}

				}

			}

			for (const auto& [s, c] : steps)
			for (const auto& [W, H] : PM_RES) {

				const auto [ratio, ox, oy] { dspace::playfield(W, H) };

				const double q{ s * ratio };
				const double units{ q * 65536. };
				const double pos_c{ c * ratio + (axis ? oy : ox) };

				const int win{ axis ? H : W };

				dspace::axis_params t{};

				t.mode = dspace::axis_mode::absolute;
				t.field.scale = ratio;
				t.field.off = axis ? oy : ox;
				t.tablet.half = (float)(win / 2);

				{ // window mapping

					const double speed{ units / win };

					float sv[2];
					const int n_speeds{ setting_values(speed, 0.01, refine, sv) };

					if (speed > 0.05 && speed < 10. && n_speeds > 0 && near_grid(pos_c - win / 2., q, 0.05)) {

						auto a{ t };

						a.tablet.mapping = dspace::tablet_mapping::window;
						a.tablet.span = (float)win;

						for (int i{}; i < n_speeds; ++i) {

							a.tablet.speed = sv[i];
							abs.push_back(a);

						}

					}

				}

				// desktop mapping
				for (const int span : SPANS) {

					const double speed{ units / span };

					float sps[2];
					const int nsp{ setting_values(speed, 0.01, refine, sps) };

					if (speed < 0.05 || speed > 10.0 || nsp == 0)
						continue;

					for (int si{}; si < nsp; ++si) {

						const float sp{ sps[si] };

						// origin = (screen x - client x)
						// pos = (L * span / 65536 + origin - half) * speed + half
						const double base{ (pos_c - t.tablet.half) / sp + t.tablet.half };
						const double step{ (double)span / 65536. };

						std::vector<int> origins;

						const size_t max_origins{ refine && si + 1 == nsp ? 1u : 4u };

						for (int o{}; origins.size() < max_origins && o <= 4000; ++o) {

							if (near_grid(base - o, step, 0.03))
								origins.push_back(o);

							if (o && origins.size() < max_origins && near_grid(base + o, step, 0.03))
								origins.push_back(-o);

						}

						for (const int o : origins) {

							auto a{ t };

							a.tablet.mapping = dspace::tablet_mapping::desktop;
							a.tablet.span = (float)span;
							a.tablet.speed = sp;
							a.tablet.origin = (float)o;

							abs.push_back(a);

						}

					}

				}

			}

			std::vector<float> tv;
			std::vector<int> tdt;
			std::vector<dotnet::text_bounds> tb;

			const auto all_informative{ informative_frames(state, vals) };

			for (const size_t i : spread_frames(all_informative, 128)) {

				tv.push_back(vals[i]);
				tdt.push_back(dt[i]);
				tb.push_back(bounds[i]);

			}

			const bool informative{ !tv.empty() };

			const auto tablet_eval = [&](const dspace::axis_params& q, size_t max_escapes) {

				if (informative)
					return prefix_eval(state, q, tv, tdt, tb, tv.size(), max_escapes);

				return prefix_eval(state, q, vals, dt, bounds, 400, max_escapes);

			};

			const size_t tablet_frames{ informative ? tv.size() : std::min<size_t>(400, vals.size()) };

			// compute heavy, only run when nothing found so far is close to acceptable
			bool clean{};

			std::vector<double> early_score(abs.size() * 2, -1.0);

			{

				constexpr size_t N{ 400 };

				for (const auto& q : cands)
					clean |= prefix_eval(state, q, vals, dt, bounds, N, (N - 1) / 100).escapes * 100 < N;

				for (size_t i{}; i < abs.size() && !clean; ++i) {

					for (u8 fp : { 0, 1 }) {

						if (clean)
							break;

						auto q{ abs[i] };

						q.tablet.fp = dspace::float_unit(fp);

						const auto r{ tablet_eval(q, (tablet_frames - 1) / 100) };

						if (std::isfinite(r.score))
							early_score[i * 2 + fp] = r.score;

						clean |= r.escapes * 100 < tablet_frames;

					}

				}

			}

			double field_lo{}, field_hi{};

			if (!clean && virtual_desktops && !p.empty()) {

				std::vector<float> sorted(p.begin(), p.end());

				const auto lo{ sorted.begin() + sorted.size() / 100 };
				const auto hi{ sorted.end() - 1 - sorted.size() / 100 };

				std::nth_element(sorted.begin(), lo, sorted.end());

				field_lo = *lo;

				std::nth_element(lo, hi, sorted.end());

				field_hi = *hi;

			}

			std::set<std::array<float, 4>> listed;

			for (const auto& b : abs)
				if (b.tablet.mapping == dspace::tablet_mapping::desktop && b.tablet.speed == 1.f)
					listed.insert({ b.field.scale, b.field.off, b.tablet.span, b.tablet.origin });

			const size_t n_std{ abs.size() };

			std::vector<virtual_group> vgroups;

			for (const auto& [W, H] : PM_RES) {

				if (clean || !virtual_desktops)
					break;

				const auto [ratio, ox, oy] { dspace::playfield(W, H) };

				const double units{ s * ratio * 65536. };

				if (!near_grid(units, 1.0, 0.05) || units < 200.)
					continue;

				const int span{ (int)std::llround(units) };
				const double q{ span / 65536. };
				const double pos_c{ c * ratio + (axis ? oy : ox) };

				const double window_lo{ field_lo * ratio + (axis ? oy : ox) };
				const double window_hi{ field_hi * ratio + (axis ? oy : ox) };
				const int o_from{ (int)std::max(-8000., std::floor(window_hi - span) - 1) };
				const int o_to{ (int)std::min(8000., std::ceil(window_lo) + 1) };

				vgroups.push_back({ abs.size(), abs.size(), ratio, axis ? oy : ox, span, o_from, o_to });

				for (int o{ o_from }; o <= o_to; ++o) {

					if (!near_grid(pos_c - o, q, 0.01))
						continue;

					dspace::axis_params a{};

					a.mode = dspace::axis_mode::absolute;
					a.field.scale = ratio;
					a.field.off = axis ? oy : ox;
					a.tablet.half = (float)((axis ? H : W) / 2);
					a.tablet.span = (float)span;
					a.tablet.speed = 1.f;
					a.tablet.origin = (float)o;

					if (listed.insert({ a.field.scale, a.field.off, a.tablet.span, a.tablet.origin }).second)
						abs.push_back(a);

				}

				vgroups.back().to = abs.size();

			}

			if (!abs.empty()) {

				std::vector<std::pair<double, u32>> scored, top8;

				const auto add = [&](double sc, u32 k) {

					scored.push_back({ sc, k });

					const std::pair<double, u32> e{ sc, k };

					if (top8.size() < 8 || e < top8.back()) {

						top8.insert(std::upper_bound(top8.begin(), top8.end(), e), e);

						if (top8.size() > 8)
							top8.pop_back();

					}

				};

				const auto cap = [&] {

					return top8.size() < 8 ? SIZE_MAX : escape_cap(top8.back().first);
				};

				for (size_t i{}; i < n_std; ++i) {

					for (u8 fp : { 0, 1 }) {

						auto q{ abs[i] };

						q.tablet.fp = dspace::float_unit(fp);

						const bool known{ early_score[i * 2 + fp] >= 0. };

						add(known ? early_score[i * 2 + fp] : tablet_eval(q, cap()).score, u32(i * 2 + fp));

					}

				}

				if (abs.size() > n_std) {

					const std::span<const float> sv{
						informative ? std::span<const float>{ tv } : vals.first(std::min<size_t>(400, vals.size()))
					};

					const std::span<const dotnet::text_bounds> sb{ informative ?
						std::span<const dotnet::text_bounds>{ tb } :
						std::span<const dotnet::text_bounds>{ bounds }.first(sv.size())
					};

					const auto fr{ lb_frames(sv, sb) };

					std::vector<std::pair<u32, u32>> by_lb;
					std::vector<u16> votes;

					for (const auto& g : vgroups) {

						const int counted{ virtual_lb(fr, g, votes) };

						for (size_t i{ g.from }; i < g.to; ++i)
							by_lb.push_back({ u32(counted - votes[(int)abs[i].tablet.origin - g.o_from]), (u32)i });

					}

					std::sort(by_lb.begin(), by_lb.end());

					for (const auto& [lb, i] : by_lb) {

						const double low{ 40. * lb };

						if (top8.size() == 8 && low > top8.back().first)
							break;

						if (top8.size() == 8 && std::pair<double, u32>{ low, i * 2 } > top8.back())
							continue;

						auto q{ abs[i] };

						q.tablet.fp = dspace::float_unit::sse;

						const double sc{ tablet_eval(q, cap()).score };

						add(sc, i * 2);
						add(sc, i * 2 + 1);

					}

				}

				std::vector<std::pair<double, u32>> order(std::move(scored));

				std::sort(order.begin(), order.end());

				dspace::axis_params best{};
				double best_score{ std::numeric_limits<double>::max() };

				std::vector<float> rv;
				std::vector<int> rdt;
				std::vector<dotnet::text_bounds> rb;

				for (const size_t i : spread_frames(all_informative, 512)) {

					rv.push_back(vals[i]);
					rdt.push_back(dt[i]);
					rb.push_back(bounds[i]);

				}

				for (size_t i{}; i < std::min<size_t>(order.size(), 8); ++i) {

					auto q{ abs[order[i].second / 2] };

					q.tablet.fp = dspace::float_unit(order[i].second % 2);

					const double sc{ rv.empty() ?
						prefix_score(state, q, vals, dt, bounds, PREFIX, best_score) :
						prefix_score(state, q, rv, rdt, rb, rv.size(), best_score)
					};

					if (sc < best_score) {

						best_score = sc;
						best = q;

					}

				}

				cands.push_back(best);

				// SSE and x87 often tie on the sampled frame so let the whole replay decide
				if (best.mode == dspace::axis_mode::absolute) {

					auto flip{ best };

					flip.tablet.fp = best.tablet.fp == dspace::float_unit::sse ? dspace::float_unit::x87 : dspace::float_unit::sse;
					cands.push_back(flip);

				}

			}

		}

		dspace::axis_params chosen{ none };
		dspace::axis_streams best_streams;

		size_t chosen_cost{ std::numeric_limits<size_t>::max() };

		std::vector<dspace::axis_params> finals;

		// the variants can repeat a candidate, each is coded once
		const auto add_final = [&](const dspace::axis_params& q) {

			if (std::find(finals.begin(), finals.end(), q) == finals.end())
				finals.push_back(q);

		};

		for (const auto& q : cands) {

			// a desktop tablet at speed 1 fits the same every span / gcd(span, 65536) - try the one nearest 0 first
			if (q.mode == dspace::axis_mode::absolute && q.tablet.mapping == dspace::tablet_mapping::desktop &&
				q.tablet.speed == 1.f && q.tablet.span >= 1.f && q.tablet.span == std::floor(q.tablet.span)) {

				const i64 span{ (i64)q.tablet.span };
				const i64 period{ span / (i64)std::gcd((u64)span, (u64)65536) };
				const i64 o{ (i64)q.tablet.origin };

				i64 c{ ((o % period) + period) % period };

				if (c * 2 > period)
					c -= period;

				if (c != o) {

					auto r{ q };

					r.tablet.origin = (float)c;
					add_final(r);

				}

			}

			add_final(q);

			// the game may keep pos - off unrounded so we have to try both (JIT differences?)
			if (q.mode == dspace::axis_mode::mouse || q.mode == dspace::axis_mode::absolute) {

				auto r{ q };

				r.field.wide = !r.field.wide;
				add_final(r);

			}

		}

		// keep q when it codes smaller
		const auto consider = [&](const dspace::axis_params& q) {

			// a model that escapes most frames is worse than text ordinals (the encode stops once it has)
			auto st{ dspace::encode_axis(q, vals, dt, state.exact_text(), bounds, vals.size() / 2) };

			if (st.escapes.size() * 2 > vals.size())
				return;

			if (const size_t cost{ device_cost(state, q, st, dt, axis) }; cost < chosen_cost) {

				chosen_cost = cost;
				chosen = q;
				best_streams = std::move(st);

			}

		};

		for (const auto& q : finals)
			consider(q);

		if (refine && chosen_cost != SIZE_MAX && (chosen.mode == dspace::axis_mode::mouse || chosen.mode == dspace::axis_mode::absolute)) {

			const auto setting = [](dspace::axis_params& q) -> float& {
				return q.mode == dspace::axis_mode::mouse ? q.mouse.sens : q.tablet.speed;
			};

			auto base{ chosen };
			const float v0{ setting(base) };

			if (!dspace::on_hundredths(v0)) {

				constexpr size_t N{ 1000 };

				double best{ prefix_score(state, chosen, vals, dt, bounds, N) };
				i64 best_j{};

				const auto at = [&](i64 j) {

					auto q{ base };

					setting(q) = std::bit_cast<float>((u32)((i64)std::bit_cast<u32>(v0) + j));

					return q;
				};

				for (const int stride : { 64, 8, 1 }) {

					const i64 from{ best_j };

					const int K{ stride == 64 ? 8 : 4 };

					for (int k{ -K }; k <= K; ++k) {

						if (k == 0)
							continue;

						const auto q{ at(from + k * stride) };

						if (const double sc{ prefix_score(state, q, vals, dt, bounds, N, best) }; sc < best) {

							best = sc;
							best_j = from + k * stride;

						}

					}

				}

				if (best_j != 0)
					consider(at(best_j));

			}

		}

		// we are escaping often, the step may be a neighbouring lattice
		if (!refine && s > 0. && (chosen.mode == dspace::axis_mode::none || best_streams.escapes.size() * 100 > vals.size())) {

			dspace::axis_streams rs;

			if (const auto r{ detect_axis_model(state, vals, dt, axis, other, rs, false, true, s, c) }; r.mode != dspace::axis_mode::none) {

				if (const size_t cost{ device_cost(state, r, rs, dt, axis) }; cost < chosen_cost) {

					chosen = r;
					best_streams = std::move(rs);

				}

			}

		}

		streams = std::move(best_streams);

		return chosen;
	}

	static void refine_window(orz_state& state, const std::vector<float> vals[2], const std::vector<int>& dt,
		dspace::axis_params& px, dspace::axis_params& py, dspace::axis_streams ws[2]) {

		dspace::axis_params* pa[2]{ &px, &py };

		const auto window_tablet = [](const dspace::axis_params& q) {
			return q.mode == dspace::axis_mode::absolute && q.tablet.mapping == dspace::tablet_mapping::window;
		};

		const int t{ window_tablet(px) ? 0 : window_tablet(py) ? 1 : -1 };

		const auto lattice = [](const dspace::axis_params& q) {
			return q.mode == dspace::axis_mode::absolute || q.mode == dspace::axis_mode::rational;
		};

		if (!lattice(px) && !lattice(py))
			return;

		if ((ws[0].escapes.size() + ws[1].escapes.size()) * 100 <= vals[0].size())
			return;

		dspace::axis_params tab{ t >= 0 ? *pa[t] : px.mode == dspace::axis_mode::absolute ? px : py };

		tab.mode = dspace::axis_mode::absolute;

		tab.tablet.mapping = dspace::tablet_mapping::window;
		tab.tablet.origin = 0.f;

		const std::span<const float> p[2]{ std::span<const float>{ vals[0] }.subspan(2), std::span<const float>{ vals[1] }.subspan(2) };

		double sx{}, sy{}, c{};

		if (t == 0)
			sx = (double)tab.tablet.speed * tab.tablet.span / 65536. / tab.field.scale;
		else if (!fit_axis_step(p[0], sx, c))
			return;

		if (t == 1)
			sy = (double)tab.tablet.speed * 480. / 65536.;
		else if (!fit_axis_step(p[1], sy, c))
			return;

		std::vector<std::pair<double, double>> ys{ { sy, c } };

		if (t != 1)
			neighbour_steps(p[1], sy, ys);
		
		std::vector<std::pair<double, float>> setups;

		for (const auto& [q, qc] : ys) {

			const double aspect{ sx / q };

			if (!(aspect > 0.2 && aspect < 5.))
				continue;

			if (t >= 0) {

				setups.push_back({ aspect, tab.tablet.speed });

				continue;
			}

			const double sp{ q * 65536. / 480. };

			if (sp < 0.05 || sp > 10.)
				continue;

			float sv[2];

			for (int i{}, n{ setting_values(sp, 0.01, true, sv) }; i < n; ++i)
				setups.push_back({ aspect, sv[i] });

		}
		
		if (t < 0) {

			for (const double aspect : { 16. / 9., 16. / 10., 4. / 3., 5. / 4., 21. / 9., 3. / 2., 5. / 3. }) {

				const double sp{ sx * 65536. / 480. / aspect };

				float sv[2];

				if (sp > 0.05 && sp < 10. && setting_values(sp, 0.01, false, sv) == 1)
					setups.push_back({ aspect, sv[0] });

			}

		}

		if (setups.empty())
			return;

		std::vector<float> tv[2];
		std::vector<int> tdt[2];
		std::vector<dotnet::text_bounds> tb[2];

		for (int a{}; a < 2; ++a) {

			auto pick{ spread_frames(informative_frames(state, vals[a]), 128) };

			if (pick.size() < 32) {

				pick.clear();

				for (size_t i{ 2 }; i < vals[a].size(); i += std::max<size_t>(1, vals[a].size() / 128))
					pick.push_back(i);

			}

			for (const size_t i : pick) {

				tv[a].push_back(vals[a][i]);
				tdt[a].push_back(dt[i]);
				tb[a].push_back(dotnet::text_bounds{ vals[a][i], state.exact_text() });

			}

		}

		const auto make = [&](int W, int H, int si, int a) {

			const auto [ratio, ox, oy] { dspace::playfield(W, H) };

			auto q{ tab };

			q.tablet.speed = setups[si].second;

			q.field.scale = ratio;
			q.field.off = a ? oy : ox;
			q.tablet.span = (float)(a ? H : W);
			q.tablet.half = (float)((a ? H : W) / 2);

			return q;
		};

		std::vector<std::pair<size_t, std::array<int, 3>>> scored;

		for (int si{}; si < (int)setups.size(); ++si) {

			const double aspect{ setups[si].first };

			for (int H{ 240 }; H <= 4320; ++H) {

				const int W{ (int)std::lround(H * aspect) };

				if (std::abs(W - H * aspect) > 0.02)
					continue;

				size_t esc{};

				for (int a{}; a < 2; ++a)
					esc += dspace::encode_axis(make(W, H, si, a), tv[a], tdt[a], state.exact_text(), tb[a]).escapes.size();

				scored.push_back({ esc, { W, H, si } });

			}

		}

		std::sort(scored.begin(), scored.end());

		size_t best_cost{};

		for (int a{}; a < 2; ++a)
			best_cost += device_cost(state, *pa[a], ws[a], dt, a);

		for (size_t i{}; i < std::min<size_t>(scored.size(), 4); ++i) {

			const auto [W, H, si] { scored[i].second };

			dspace::axis_params q[2];
			dspace::axis_streams st[2];
			size_t cost{};

			for (int a{}; a < 2; ++a) {

				q[a] = make(W, H, si, a);
				st[a] = dspace::encode_axis(q[a], vals[a], dt, state.exact_text(), {}, vals[a].size() / 2);

				if (st[a].escapes.size() * 2 > vals[a].size())
					cost = SIZE_MAX;

				if (cost != SIZE_MAX)
					cost += device_cost(state, q[a], st[a], dt, a);

			}

			if (cost < best_cost) {

				best_cost = cost;

				for (int a{}; a < 2; ++a) {

					*pa[a] = q[a];
					ws[a] = std::move(st[a]);

				}

			}

		}

	}

	// get a lot of escapes when the window moves or the input device swaps, etc
	// so each 250 frame chunk is detected on its own
	constexpr size_t SEGMENT_CHUNK{ 250 };

	static size_t escapes_of(const dspace::axis_params& p, std::span<const float> vals, std::span<const int> dt,
		size_t from, size_t to, bool exact_text) {
		return dspace::encode_axis(p, vals.subspan(from, to - from), dt.subspan(from, to - from), exact_text).escapes.size();
	}

	constexpr size_t SEGMENT_GIVE_UP{ 4 };
	constexpr size_t SEGMENT_REUSE_THRESHOLD{ 200 };

	static dspace::axis_model segment_axis(orz_state& state, std::span<const float> vals, std::span<const int> dt, int axis,
		const dspace::axis_model* other, bool give_up_early, bool virtual_desktops) {

		const size_t n{ vals.size() };
		dspace::axis_model m;
		size_t misses_in_row{}, other_at{};

		for (size_t from{}; from < n; ) {

			size_t to{ std::min(n, from + SEGMENT_CHUNK) };

			if (n - to < SEGMENT_CHUNK / 3)
				to = n;

			const bool reuse{
				!m.seg.empty() && m.seg.back().p.mode != dspace::axis_mode::none &&
				escapes_of(m.seg.back().p, vals, dt, from, to, state.exact_text()) * 1000 <= (to - from) * SEGMENT_REUSE_THRESHOLD
			};

			dspace::axis_streams unused;

			const dspace::axis_params p{ reuse ?
				m.seg.back().p :
				detect_axis_model(state, vals.subspan(from, to - from), dt.subspan(from, to - from), axis,
					other ? &other->at(from, other_at) : nullptr, unused, virtual_desktops)
			};

			// a new device: the chunks before it take it too while it codes them at least as small
			if (p.mode != dspace::axis_mode::none) {

				const auto cost = [&](const dspace::axis_params& q, size_t a, size_t b) {

					return device_cost(state, q, dspace::encode_axis(q, vals.subspan(a, b - a), dt.subspan(a, b - a), state.exact_text()),
						dt.subspan(a, b - a), axis);
				};

				for (size_t k{ m.seg.size() }; k-- > 0 && m.seg[k].p != p;) {

					const size_t a{ m.seg[k].start }, b{ k + 1 < m.seg.size() ? (size_t)m.seg[k + 1].start : from };

					if (cost(p, a, b) > cost(m.seg[k].p, a, b))
						break;

					m.seg[k].p = p;

				}

			}

			m.seg.push_back({ (u32)from, p });

			from = to;

			misses_in_row = p.mode == dspace::axis_mode::none ? misses_in_row + 1 : 0;

			if (give_up_early && misses_in_row >= SEGMENT_GIVE_UP && from < n) {

				m.seg.push_back({ (u32)from, dspace::axis_params{} });

				break;
			}

		}

		const auto seg_end = [&](size_t k) {

			return k + 1 < m.seg.size() ? (size_t)m.seg[k + 1].start : n;
		};

		// chunks without a model borrow a neighbours parameters when they fit at least half the frames
		for (int pass{}; pass < 2; ++pass) {

			for (size_t k{}; k < m.seg.size(); ++k) {

				if (m.seg[k].p.mode != dspace::axis_mode::none)
					continue;

				const size_t from{ m.seg[k].start }, to{ seg_end(k) };

				for (const size_t nb : { k - 1, k + 1 }) {

					if (nb >= m.seg.size() || m.seg[nb].p.mode == dspace::axis_mode::none)
						continue;

					if (escapes_of(m.seg[nb].p, vals, dt, from, to, state.exact_text()) * 2 < to - from) {

						m.seg[k].p = m.seg[nb].p;

						break;
					}

				}

			}

		}

		{ // merge equal models

			std::vector<dspace::axis_segment> merged;

			for (const auto& sg : m.seg)
				if (merged.empty() || merged.back().p != sg.p)
					merged.push_back(sg);

			m.seg.swap(merged);

		}

		for (size_t k{ 1 }; k < m.seg.size(); ++k) {

			const auto& A{ m.seg[k - 1].p };
			const auto& B{ m.seg[k].p };

			const size_t b{ m.seg[k].start };

			const size_t w0{ std::max((size_t)m.seg[k - 1].start, b > SEGMENT_CHUNK ? b - SEGMENT_CHUNK : 0) };
			const size_t w1{ std::min(seg_end(k), b + SEGMENT_CHUNK) };

			if (w1 <= w0 + 2)
				continue;

			std::vector<int> missA(w1 - w0, 0), missB(w1 - w0, 0);

			{

				const auto v{ vals.subspan(w0, w1 - w0) };
				const auto d{ dt.subspan(w0, w1 - w0) };

				for (const auto& [f, x] : dspace::encode_axis(A, v, d, state.exact_text()).escapes)
					missA[f] = 1;

				for (const auto& [f, x] : dspace::encode_axis(B, v, d, state.exact_text()).escapes)
					missB[f] = 1;

			}

			i64 best{ std::numeric_limits<i64>::max() };

			size_t best_t{ b };
			i64 left{}, right{};

			for (const int x : missB)
				right += x;

			for (size_t t{ w0 + 1 }; t < w1; ++t) {

				left += missA[t - 1 - w0];
				right -= missB[t - 1 - w0];

				if (left + right < best) {

					best = left + right;
					best_t = t;

				}

			}

			m.seg[k].start = (u32)best_t;

		}

		{ // drop segments that became empty, merge again

			std::vector<dspace::axis_segment> clean;

			for (size_t k{}; k < m.seg.size(); ++k) {

				if (k + 1 < m.seg.size() && m.seg[k + 1].start <= m.seg[k].start)
					continue;

				if (!clean.empty() && clean.back().p == m.seg[k].p)
					continue;

				clean.push_back(m.seg[k]);

			}

			clean[0].start = 0;

			m.seg.swap(clean);

		}

		return m;
	}

}

}
