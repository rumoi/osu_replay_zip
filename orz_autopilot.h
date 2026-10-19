#pragma once

// osu!standard autopilot cursor paths. With autopilot the game moves the cursor along its own autoplay replay: knots at
// the notes, holds and linear moves between them, and in-out cubic moves sampled every 60th of a second of game time.
// A recorded frame sits at the linear interpolation of the two knots around its time, sent to the screen and back to
// the field. The knots are coded as events in the frame stream (orz_frames.h); frames they cover are predicted exactly.

#include <algorithm>
#include <cmath>
#include <span>
#include <vector>

#include "device_space.h"

namespace orz {

namespace autopilot {

	// game ms between eased samples (1000 / 60 at the replay's speed): normal, double time, half time
	constexpr double RATES[]{ 1.0, 1.5, 0.75 };
	constexpr u32 RATE_COUNT{ 3 };

	// limits for untrusted input: moves longer than these are not autopilot moves (an eased one longer is taken as
	// straight), knots kept around a frame's time. Eased knots step a float time, which holds whole ms below FLOAT_MS.
	constexpr i64 MAX_MOVE_MS{ 1 << 16 }, MAX_EASE_MS{ 1 << 12 }, FLOAT_MS{ 1 << 24 };
	constexpr size_t KEEP_KNOTS{ 1 << 12 };

	struct _params {

		bool on;
		u32 rate; // RATES index
		u32 width, height; // game window size

		float sample_ms() const {
			return (float)(1000.0 / 60.0 * RATES[rate < RATE_COUNT ? rate : 0]);
		}

	};

	// the field to the screen and back, as the game records the cursor. The game runs on the x87 unit at double precision,
	// so 1 / ratio is not rounded to a float before it scales.
	struct _screen {

		float ratio, off[2];
		double inv;

		explicit _screen(const _params& p) {

			const auto f{ dspace::playfield((int)p.width, (int)p.height) };

			ratio = f.ratio;
			off[0] = f.off_x;
			off[1] = f.off_y;
			inv = 1.0 / f.ratio;

		}

		FORCEINLINE float field(float v, int a) const {

			float s{ v * ratio };

			s = s + off[a];
			s = s - off[a];

			return (float)(s * inv);
		}

	};

	struct knot {
		i64 t;
		float p[2];
	};

	// the game's in-out cubic easing, in doubles
	FORCEINLINE double in_out_cubic(double time, double initial, double change, double duration) {

		if (change == 0 || time == 0 || duration == 0)
			return initial;

		if (time == duration)
			return initial + change;

		if ((time /= duration / 2) < 1)
			return change / 2 * time * time * time + initial;

		time -= 2;

		return change / 2 * (time * time * time + 2) + initial;
	}

	// positions stepped by whole floats (ulps), for positions only known to 7 digits
	FORCEINLINE float ulp_step(float v, i64 n) {

		if (n == 0 || !std::isfinite(v))
			return v;

		const i32 b{ std::bit_cast<i32>(v) };
		const i64 m{ b < 0 ? (i64)INT_MIN - b : b }; // ascending with the value

		const i64 s{ std::clamp<i64>(m + n, (i64)INT_MIN + 1, INT_MAX) };

		return std::bit_cast<float>((i32)(s < 0 ? (i64)INT_MIN - s : s));
	}

	// the last three points at different times (knots, or new positions), oldest first
	struct trail {

		knot p[3]{};
		int n{};

		void push(const knot& q) {

			if (n && q.t == p[2].t)
				return;

			p[0] = p[1];
			p[1] = p[2];
			p[2] = q;
			n = std::min(n + 1, 3);

		}

		// the line through the last two, at time t
		float line(i64 t, int a) const {

			if (n < 2)
				return p[2].p[a];

			const double g{ (double)p[2].p[a] + (double)(p[2].p[a] - p[1].p[a]) * (double)(t - p[2].t) / (double)(p[2].t - p[1].t) };

			return std::abs(g) < 1e7 ? (float)g : p[2].p[a];
		}

		// where a target likely is: the last step carried on to time t, turned as much as the step before it turned
		// (slider paths are smooth)
		float guess(i64 t, int a) const {

			if (n < 3)
				return line(t, a);

			double v[2], u[2];

			for (int x{}; x < 2; ++x) {

				v[x] = ((double)p[2].p[x] - p[1].p[x]) / (double)(p[2].t - p[1].t);
				u[x] = ((double)p[1].p[x] - p[0].p[x]) / (double)(p[1].t - p[0].t);

			}

			const double uu{ u[0] * u[0] + u[1] * u[1] };

			if (!(uu > 1e-9))
				return line(t, a);

			// v * (v / u) as complex numbers, kept to plain turns
			const double r[2]{ (v[0] * u[0] + v[1] * u[1]) / uu, (v[1] * u[0] - v[0] * u[1]) / uu };
			const double rr{ r[0] * r[0] + r[1] * r[1] };

			if (!(rr > 0.25 && rr < 4))
				return line(t, a);

			const double w{ a ? v[0] * r[1] + v[1] * r[0] : v[0] * r[0] - v[1] * r[1] };
			const double g{ (double)p[2].p[a] + w * (double)(t - p[2].t) };

			return std::abs(g) < 1e7 ? (float)g : p[2].p[a];
		}

	};

	enum class kind : u8 { line, ease };

	// a move from the anchor (the last knot, or the last frame when knots ran out) to a target, after a hold
	struct move {

		kind type;
		i64 wait, time;  // hold from the anchor's time, then the move's length
		float from[2];    // anchor position (from the last frame: stepped by ulps)
		float to[2];

	};

	struct path {

		std::vector<knot> k;
		size_t at{};
		size_t budget{ SIZE_MAX }; // eased knots left to make (bounds untrusted input)

		bool covers(i64 t) const {
			return !k.empty() && k.back().t >= t;
		}

		// the last three knots at different times
		trail tail() const {

			trail r{};
			knot q[3];
			int n{};

			for (size_t j{ k.size() }; j > 0 && n < 3; --j)
				if (!n || k[j - 1].t != q[n - 1].t)
					q[n++] = k[j - 1];

			while (n)
				r.push(q[--n]);

			return r;
		}

		// the linear interpolation between the knots around t, as the game plays a replay back
		void predict(i64 t, float (&out)[2]) {

			const auto it{ std::upper_bound(k.begin(), k.end(), t, [](i64 v, const knot& q) { return v < q.t; }) };

			at = it == k.begin() ? 0 : (size_t)(it - k.begin()) - 1;

			const knot& a{ k[at] };

			if (a.t > t || at + 1 >= k.size()) {

				out[0] = a.p[0];
				out[1] = a.p[1];

				return;
			}

			const knot& b{ k[at + 1] };

			const float u{ std::max(0.f, 1.f - (float)(b.t - t) / (float)(b.t - a.t)) };

			for (int x{}; x < 2; ++x)
				out[x] = a.p[x] + (b.p[x] - a.p[x]) * u;

		}

		void add(const move& m, i64 anchor_t, bool anchor_is_knot, float sample_ms) {

			// drop knots far behind, both ends do the same
			if (at > 2 * KEEP_KNOTS) {

				k.erase(k.begin(), k.begin() + (at - KEEP_KNOTS));
				at = KEEP_KNOTS;

			}

			if (!anchor_is_knot)
				k.push_back({ anchor_t, { m.from[0], m.from[1] } });

			const i64 t0{ anchor_t + m.wait }, t1{ t0 + m.time };

			if (m.wait > 0)
				k.push_back({ t0, { m.from[0], m.from[1] } });

			if (m.type == kind::ease && m.time <= MAX_EASE_MS && std::abs(t0) < FLOAT_MS && std::abs(t1) < FLOAT_MS) {

				// the autoplay replay samples an eased move every sample_ms from its start, a float time truncated to whole ms
				const float end{ (float)t1 };

				for (float time{ (float)t0 + sample_ms }; time < end && budget; time = time + sample_ms, --budget) {

					const float current{ (float)((double)time - (double)t0) }, duration{ (float)((double)end - (double)t0) };

					knot q{ (i64)time };

					for (int x{}; x < 2; ++x)
						q.p[x] = (float)in_out_cubic(current, m.from[x], (float)(m.to[x] - m.from[x]), duration);

					k.push_back(q);

				}

			}

			k.push_back({ t1, { m.to[0], m.to[1] } });

		}

	};

	// a move as coded: targets are integers from the anchor's rounded position, or 7 digit ordinals from the anchor's
	// stepped by ulps; an anchor from the last frame is stepped by ulps too
	struct event {

		u32 sample;
		kind type;
		i64 wait, time;
		i64 from_ulp[2];
		bool to_int[2];
		i64 to[2], to_ulp[2];

	};

	constexpr i64 MAX_INT{ 1 << 24 }, MAX_ORD{ 1ll << 40 }, MAX_ULP{ 1 << 20 };

	inline move resolve(const event& e, const float (&anchor)[2], bool anchor_is_knot, i64 anchor_t, const trail& ref) {

		move m{ e.type, std::clamp<i64>(e.wait, 0, MAX_MOVE_MS), std::clamp<i64>(e.time, 1, MAX_MOVE_MS) };

		for (int a{}; a < 2; ++a) {

			m.from[a] = anchor_is_knot ? anchor[a] : ulp_step(anchor[a], std::clamp<i64>(e.from_ulp[a], -MAX_ULP, MAX_ULP));

			if (e.to_int[a]) {

				const i64 base{ std::isfinite(m.from[a]) ? std::clamp<i64>(std::llround(std::clamp(m.from[a], -1e7f, 1e7f)), -MAX_INT, MAX_INT) : 0 };

				m.to[a] = (float)std::clamp<i64>(base + std::clamp<i64>(e.to[a], -MAX_INT, MAX_INT), -MAX_INT, MAX_INT);

			} else {

				i64 o{};

				if (!dotnet::text_ord(ref.guess(anchor_t + m.wait + m.time, a), false, o))
					o = 0;

				m.to[a] = ulp_step(dotnet::from_text_ord(std::clamp<i64>(o + std::clamp<i64>(e.to[a], -MAX_ORD, MAX_ORD), -MAX_ORD, MAX_ORD), false),
					std::clamp<i64>(e.to_ulp[a], -MAX_ULP, MAX_ULP));

			}

		}

		return m;
	}

	// encoder: the frames that are not repeats, with their times and 7 digit ordinals
	struct sample {
		i64 t;
		i64 K[2];
		float v[2];
		bool stays; // the next frame repeats it
	};

	// a predicted field position prints as the frame's
	FORCEINLINE bool lands(const _screen& screen, const float (&v)[2], const sample& q) {

		i64 x, y;

		return dotnet::text_ord(screen.field(v[0], 0), false, x) && dotnet::text_ord(screen.field(v[1], 1), false, y) && x == q.K[0] && y == q.K[1];
	}

	// which moves to code, played the way the frame loop plays them: a move is coded at the first frame past the knots
	// and kept when the frames it predicts from there would cost more than it does
	struct fitter {

		static constexpr size_t TARGETS{ 40 }, NEAR_TARGETS{ 4 }, FAR_TARGETS{ 3 };

		// knots built and frames tried per frame before it stops looking for moves (real replays stay far below)
		static constexpr size_t WORK{ 1 << 12 };

		std::span<const sample> s;
		_screen screen;
		float sample_ms;

		// about what a new position costs off the knots (the line through the last two), in bits, and summed
		std::vector<int> off_sum;

		mutable size_t work{};

		// rough code length of a value
		static int bits(i64 v) {
			return v == 0 ? 1 : 2 * (64 - std::countl_zero((u64)(v < 0 ? -v : v))) + 2;
		}

		fitter(std::span<const sample> s_, const _params& p) : s{ s_ }, screen{ p }, sample_ms{ p.sample_ms() } {

			off_sum.assign(s.size() + 1, 0);

			trail seen{};

			for (size_t i{}; i < s.size(); ++i) {

				int c{};

				for (int a{}; a < 2; ++a) {

					i64 o{ s[i - (i > 0)].K[a] };

					if (seen.n >= 2)
						dotnet::text_ord(seen.line(s[i].t, a), false, o);

					c += bits(s[i].K[a] - o);

				}

				off_sum[i + 1] = off_sum[i] + c;

				seen.push({ s[i].t, { s[i].v[0], s[i].v[1] } });

			}

		}

		// rough code length of a move
		static int cost(const event& e) {

			int c{ 4 + bits(e.wait) + bits(e.time) };

			for (int a{}; a < 2; ++a)
				c += 1 + bits(e.to[a]) + (e.to_int[a] ? 0 : bits(e.to_ulp[a]));

			return c;
		}

		// frames from i the move predicts in a row, per axis (the move covers `within` of them)
		void run(i64 anchor_t, const move& m, size_t i, size_t (&n)[2], size_t& within) const {

			path tr{};

			tr.k.push_back({ anchor_t, { m.from[0], m.from[1] } });
			tr.add(m, anchor_t, true, sample_ms);

			bool on[2]{ true, true };

			n[0] = n[1] = within = 0;
			work += tr.k.size();

			for (size_t j{ i }; j < s.size() && tr.covers(s[j].t) && (on[0] || on[1]); ++j, ++within, ++work) {

				float v[2];

				tr.predict(s[j].t, v);

				for (int a{}; a < 2; ++a) {

					i64 o;

					on[a] = on[a] && dotnet::text_ord(screen.field(v[a], a), false, o) && o == s[j].K[a];
					n[a] += on[a];

				}

			}

		}

		size_t run(i64 anchor_t, const move& m, size_t i) const {

			size_t n[2], within;

			run(anchor_t, m, i, n, within);

			return std::min(n[0], n[1]);
		}

		// ulp steps on the anchor (when it is a frame) and on targets off whole positions, per axis, for a move that misses
		void refine(event& e, const knot& anchor, bool is_knot, const trail& ref, size_t i) const {

			constexpr i64 STEPS{ 4 };

			for (const auto field : { &event::from_ulp, &event::to_ulp }) {

				for (int a{}; a < 2; ++a) {

					if (field == &event::from_ulp ? is_knot : e.to_int[a])
						continue;

					size_t n[2], within, best;

					run(anchor.t, resolve(e, anchor.p, is_knot, anchor.t, ref), i, n, within);

					if (n[a] >= within)
						continue;

					best = n[a];

					const i64 base{ (e.*field)[a] };

					for (i64 u{ -STEPS }; u <= STEPS; ++u) {

						event q{ e };

						(q.*field)[a] = base + u;

						run(anchor.t, resolve(q, anchor.p, is_knot, anchor.t, ref), i, n, within);

						if (n[a] > best) {

							best = n[a];
							e = q;

						}

					}

				}

			}

		}

		// how far along a move of the given type is when it has covered this fraction of the way
		static double progress(kind type, double f) {

			if (type == kind::line)
				return f;

			double lo{}, hi{ 1 };

			for (int k{}; k < 24; ++k) {

				const double mid{ (lo + hi) / 2 };

				(in_out_cubic(mid, 0, 1, 1) < f ? lo : hi) = mid;

			}

			return (lo + hi) / 2;
		}

		// the fraction of the way from a to b that v is at, on the axis that moves most
		static double fraction(const float (&a)[2], const float (&b)[2], const float (&v)[2]) {

			const int x{ std::abs(b[1] - a[1]) > std::abs(b[0] - a[0]) ? 1 : 0 };
			const double span{ (double)b[x] - a[x] };

			return span == 0 ? -1 : ((double)v[x] - a[x]) / span;
		}

		// a move to the target frame's position
		event make(u32 at, kind type, i64 wait, i64 time, const knot& anchor, const trail& ref, const sample& target) const {

			event e{ at, type, wait, time };

			const float (&from)[2]{ anchor.p };

			for (int a{}; a < 2; ++a) {

				const float v{ target.v[a] };

				e.to_int[a] = v == std::floor(v) && std::abs(v) < (float)MAX_INT;

				if (e.to_int[a])
					e.to[a] = (i64)v - std::clamp<i64>(std::llround(std::clamp(from[a], -1e7f, 1e7f)), -MAX_INT, MAX_INT);
				else {

					i64 o{};

					dotnet::text_ord(ref.guess(anchor.t + wait + time, a), false, o);

					e.to[a] = target.K[a] - o;

				}

			}

			return e;
		}

		std::vector<event> fit(size_t& covered) {

			std::vector<event> out;
			path pth{};

			bool prev_covered{};

			// the last new positions at different times
			trail seen{};

			covered = 0;

			for (size_t i{}; i < s.size(); ++i) {

				if (!pth.covers(s[i].t) && i > 0 && work < WORK * s.size()) {

					const bool is_knot{ prev_covered && !pth.k.empty() };
					const knot anchor{ is_knot ? pth.k.back() : knot{ s[i - 1].t, { s[i - 1].v[0], s[i - 1].v[1] } } };
					const trail ref{ is_knot ? pth.tail() : seen };

					event best{}, longest{};
					int best_net{};
					size_t longest_n{};

					size_t far{};

					for (size_t j{ i }; j < s.size() && j < i + TARGETS && s[j].t > anchor.t && far < FAR_TARGETS; ++j) {

						if (j >= i + NEAR_TARGETS) {

							if (!s[j].stays && !(s[j].v[0] == std::floor(s[j].v[0]) && s[j].v[1] == std::floor(s[j].v[1])))
								continue;

							++far;

						}

						// the arrival is at the target frame or between it and the frame before
						const i64 lo{ std::max(anchor.t, j > i ? s[j - 1].t : s[i - 1].t) + 1 }, hi{ s[j].t };

						for (const kind type : { kind::line, kind::ease }) {

							// start and arrival from where the first and last frames that moved are
							i64 t0s[6]{ anchor.t }, t1s[6]{ hi };
							int n0{ 1 }, n1{ 1 };

							if (j > i) {

								const double ui{ progress(type, fraction(anchor.p, s[j].v, s[i].v)) },
									ul{ progress(type, fraction(anchor.p, s[j].v, s[j - 1].v)) };

								double t0{ -1e18 }, t1{ (double)hi };

								if (j - 1 > i && ul > ui && ul < 1 && s[j - 1].t > s[i].t) {

									const double rate{ (ul - ui) / (double)(s[j - 1].t - s[i].t) };

									t0 = (double)s[i].t - ui / rate;
									t1 = (double)s[i].t + (1 - ui) / rate;

								} else if (ui > 0 && ui < 1)
									t0 = ((double)s[i].t - ui * t1) / (1 - ui);

								if (std::abs(t1) < 1e15)
									for (int d{ -2 }; d <= 2; ++d)
										if (const i64 c{ std::llround(t1) + d }; c >= lo && c < hi)
											t1s[n1++] = c;

								if (std::abs(t0) < 1e15)
									for (int d{ -2 }; d <= 2; ++d)
										if (const i64 c{ std::llround(t0) + d }; c > anchor.t && c < s[i].t)
											t0s[n0++] = c;

							}

							for (int x{}; x < n1; ++x) {

								for (int y{}; y < n0; ++y) {

									const i64 t1{ t1s[x] }, t0{ t0s[y] };

									if (t0 >= t1 || t1 - t0 > (type == kind::ease ? MAX_EASE_MS : MAX_MOVE_MS))
										continue;

									const event e{ make((u32)i, type, t0 - anchor.t, t1 - t0, anchor, ref, s[j]) };
									const size_t n{ run(anchor.t, resolve(e, anchor.p, true, anchor.t, ref), i) };
									const int net{ off_sum[i + n] - off_sum[i] - 2 * (int)n - cost(e) };

									if (n > longest_n) {

										longest_n = n;
										longest = e;

									}

									if (net > best_net) {

										best_net = net;
										best = e;

									}

								}

							}

						}

					}

					// the longest move, refined, against the best one
					if (longest_n) {

						event q{ longest };

						refine(q, anchor, is_knot, ref, i);

						const size_t n{ run(anchor.t, resolve(q, anchor.p, is_knot, anchor.t, ref), i) };
						const int net{ off_sum[i + n] - off_sum[i] - 2 * (int)n - cost(q) };

						if (net > best_net) {

							best_net = net;
							best = q;

						}

					}

					if (best_net > 0) {

						out.push_back(best);
						pth.add(resolve(best, anchor.p, is_knot, anchor.t, ref), anchor.t, is_knot, sample_ms);

					}

				}

				seen.push({ s[i].t, { s[i].v[0], s[i].v[1] } });

				if (!pth.covers(s[i].t)) {

					prev_covered = false;

					continue;
				}

				float v[2];

				pth.predict(s[i].t, v);

				covered += lands(screen, v, s[i]);
				prev_covered = true;

			}

			return out;
		}

	};

	// the speed and window that make the moves between consecutive stops at whole positions exact (where autopilot goes
	// note to note, eased or straight); off when no setting makes enough frames exact (a played cursor makes far fewer)
	inline _params detect(std::span<const sample> s, const std::pair<u32, u32>* windows, size_t window_count) {

		constexpr size_t PAIRS{ 64 }, MIN_HITS{ 64 };

		const auto whole = [&](size_t i) { return s[i].v[0] == std::floor(s[i].v[0]) && s[i].v[1] == std::floor(s[i].v[1]); };

		std::vector<std::pair<size_t, size_t>> pairs;

		for (size_t i{}, last{ SIZE_MAX }; i < s.size() && pairs.size() < PAIRS; ++i) {

			if (!whole(i))
				continue;

			if (last != SIZE_MAX && i > last + 1 && i - last <= 40 && s[i].t > s[last].t)
				pairs.push_back({ last, i });

			last = i;

		}

		// frames between the stops a setting makes exact, taking the better of straight and eased per pair
		const auto hits = [&](const _params& p) {

			const _screen screen{ p };

			size_t total{};

			for (const auto& [a, b] : pairs) {

				size_t most{};

				for (const kind type : { kind::line, kind::ease }) {

					path tr{};

					tr.k.push_back({ s[a].t, { s[a].v[0], s[a].v[1] } });
					tr.add({ type, 0, s[b].t - s[a].t, { s[a].v[0], s[a].v[1] }, { s[b].v[0], s[b].v[1] } }, s[a].t, true, p.sample_ms());

					size_t n{};

					for (size_t j{ a + 1 }; j < b; ++j) {

						float v[2];

						tr.predict(s[j].t, v);

						n += lands(screen, v, s[j]);

					}

					most = std::max(most, n);

				}

				total += most;

			}

			return total;
		};

		// a played cursor makes few exact moves at the most common window too: the others are only tried past half of
		// MIN_HITS there
		size_t first{};

		for (u32 rate{}; rate < RATE_COUNT; ++rate)
			first = std::max(first, hits({ true, rate, windows[0].first, windows[0].second }));

		if (first * 2 < MIN_HITS)
			return {};

		_params best{};
		size_t best_hits{};

		for (u32 rate{}; rate < RATE_COUNT; ++rate) {

			for (size_t w{}; w < window_count; ++w) {

				const _params p{ true, rate, windows[w].first, windows[w].second };

				if (const size_t h{ hits(p) }; h > best_hits) {

					best_hits = h;
					best = p;

				}

			}

		}

		if (best_hits < MIN_HITS)
			best = {};

		return best;
	}

}

}
