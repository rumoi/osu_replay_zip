#pragma once

// the setup behind an osu!standard coding model. Several models give the same positions: a tablet mapped to the window
// at speed 2 is one mapped to a desktop twice as wide, a driver with half the output range doubles every speed. What
// they share is where a tablet count lands in the window, so the setup is stated in those terms.

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <vector>

#include "device_space.h"

namespace orz {

namespace setup {

	// the window size an axis's field mapping belongs to, 0 if it is none
	inline u32 window_of_axis(const dspace::axis_params& p) {
		return (u32)dspace::window_size(p);
	}

	// tablet axis: window pixel of coded integer K is K * quantum + at_zero
	struct _tablet_line {
		double quantum, at_zero;
	};

	inline _tablet_line tablet_line(const dspace::axis_params& p) {

		const double span{ p.device.absolute.span }, speed{ p.device.absolute.speed }, half{ p.game.window_half };

		if (p.device.absolute.mapping == dspace::tablet_mapping::window)
			return { speed * span / 65536., span * (1. - speed) / 2. };

		return { span * speed / 65536., ((double)p.game.origin - half) * speed + half };
	}

	FORCEINLINE bool whole(double v) {
		return v > 0. && std::abs(v - std::round(v)) < 0.05;
	}

	// A window starts on a whole pixel of its desktop. Shifting every coded integer moves it by that many of the pixels
	// one integer covers (reach / 65536), so a start read off another form of the same device can sit between two
	// pixels: the first whole one from the desktop's start that a whole shift reaches, or the start as read.
	inline double whole_start(double at, double reach) {

		if (std::abs(at - std::round(at)) < 1e-6 || !(reach >= 1.) || reach > 65536.)
			return at;

		for (double to{}; to <= reach; to += 1.)
			if (const double shift{ (at - to) * 65536. / reach }; std::abs(shift - std::round(shift)) < 1e-6)
				return to;

		return at;
	}

	// the in-game sensitivity a tablet's two axes stand for, 1 when they do not agree on another: the same factor between
	// the pixels the coded integers cover and the window on both axes, and too little over the window to be a second
	// monitor beside it. reach, window: per axis.
	inline float tablet_sensitivity(const double reach[2], const double window[2]) {

		if (!(window[0] > 0.) || !(window[1] > 0.) || !(reach[0] > 0.) || !(reach[1] > 0.))
			return 1.f;

		const double rx{ reach[0] / window[0] }, ry{ reach[1] / window[1] };

		if (std::abs(rx - ry) >= 0.004 * rx || std::abs(rx - 1.) <= 0.004)
			return 1.f;

		if (rx > 1. && reach[0] - window[0] >= 640.)
			return 1.f;

		// settings are in hundredths
		if (std::abs(rx * 100. - std::round(rx * 100.)) > 0.03)
			return 1.f;

		return (float)(0.01 * std::round(rx * 100.));
	}

	// What whole displays span along an axis, and how unlikely each is: nothing, a common display's side (the commoner
	// the display the likelier, PM_RES's order, and its own way up before turned), or two of them together.
	struct _display_span {
		i64 pixels;
		int cost;
	};

	static const auto DISPLAY_SPANS = [] {

		constexpr int TURNED{ 8 }, SECOND{ 4 };

		std::array<std::vector<_display_span>, 2> spans;

		for (int a{}; a < 2; ++a) {

			std::vector<_display_span> one;

			for (int i{}; i < (int)std::size(dspace::PM_RES); ++i) {

				one.push_back({ dspace::PM_RES[i][a], 1 + i });
				one.push_back({ dspace::PM_RES[i][1 - a], 1 + i + TURNED });

			}

			auto& all{ spans[a] };

			all = one;
			all.push_back({ 0, 0 });

			for (const auto& x : one)
				for (const auto& y : one)
					all.push_back({ x.pixels + y.pixels, x.cost + y.cost + SECOND });

			// the likeliest way to span each
			std::sort(all.begin(), all.end(), [](const _display_span& x, const _display_span& y) {

				return x.pixels != y.pixels ? x.pixels < y.pixels : x.cost < y.cost;
			});

			all.erase(std::unique(all.begin(), all.end(), [](const _display_span& x, const _display_span& y) {

				return x.pixels == y.pixels;
			}), all.end());

		}

		return spans;
	}();

	// Where a tablet's window likely is on an axis. The replay gives its position up to a period (window_period), and
	// windows are not just anywhere: the game fills a display, which starts the desktop or has whole displays either
	// side of it (the fewest and commonest that fit), or it is letterboxed in the middle of one.
	inline void place_window(dspace::_setup& s, int a) {

		using placing = dspace::_setup::placing;

		// more than a common display either side, one of them turned, is no likelier than anywhere
		constexpr int COST_MAX{ 24 };

		s.window_likely[a] = s.window_at[a];
		s.window_placed[a] = placing::coded;

		// the pixels of the desktop the window leaves free, and the first position from its start
		const i64 free{ (i64)s.desktop[a] - (i64)s.window[a] }, period{ s.window_period[a] };

		if (s.to_window[a] || !s.window[a] || free < 0 || period < 1)
			return;

		const i64 first{ (((i64)s.window_at[a] % period) + period) % period };

		const auto put = [&](i64 at, placing how) {

			s.window_likely[a] = (i32)at;
			s.window_placed[a] = how;

		};

		if (first == 0)
			return put(0, placing::start);

		if (first > free)
			return;

		if (first + period > free)
			return put(first, placing::only);

		const auto& spans{ DISPLAY_SPANS[a] };

		int least{ COST_MAX + 1 };

		for (const auto& before : spans) {

			if (before.pixels < first || before.pixels > free || (before.pixels - first) % period != 0 || before.cost >= least)
				continue;

			const auto after{ std::lower_bound(spans.begin(), spans.end(), free - before.pixels, [](const _display_span& x, i64 pixels) {

				return x.pixels < pixels;
			}) };

			if (after != spans.end() && after->pixels == free - before.pixels && before.cost + after->cost < least) {

				least = before.cost + after->cost;
				put(before.pixels, placing::edge);

			}

		}

		if (least > COST_MAX && free % 2 == 0 && free / 2 >= first && (free / 2 - first) % period == 0)
			put(free / 2, placing::centred);

	}

	// a setup as a prior for an encode: its display, driver and client as each axis's device, its tablet lattice as the
	// native grid's step. The parts are taken apart: a device that no longer fits is searched again, an area that no
	// longer fits is fit again.
	inline dspace::device_hint hint(const dspace::_setup& s) {

		using input = dspace::_setup::input;

		dspace::device_hint h{};

		// whole pixels: the lattice of one pixel in field units, offset by where the playfield starts. Only the height
		// is known, and the width only matters through being even, as nearly every window's is.
		if (s.kind == input::pixels && s.window[1]) {

			const int H{ (int)s.window[1] };
			const int g{ (int)std::gcd(H, 480) };
			const int D{ H / g }, A{ 480 / g };

			const auto field{ dspace::playfield(s.window[0] ? (int)s.window[0] : 2 * H, H) };

			for (int a{}; a < 2; ++a) {

				// value = (pixel - off) * A / D, so b = -A * off, kept within one step
				const i64 b{ (i64)std::llround(-(double)A * (double)(a ? field.off_y : field.off_x)) };

				dspace::axis_params q{};

				q.device.kind = dspace::axis_mode::rational;
				q.device.rational.D = D;
				q.device.rational.a = A;
				q.device.rational.b = (int)(((b % A) + A) % A);

				h.axis[a] = q;
				h.missed[a] = s.missed[a];

			}

			return h;
		}

		if (!s.window[0] || !s.window[1])
			return h;

		const auto field{ dspace::playfield((int)s.window[0], (int)s.window[1]) };

		for (int a{}; a < 2; ++a) {

			dspace::axis_params q{};

			q.game.scale = field.ratio;
			q.game.off = a ? field.off_y : field.off_x;
			q.game.unrounded_sub = s.unrounded_sub[a];

			if (s.kind == input::mouse) {

				q.device.kind = dspace::axis_mode::mouse;
				q.device.mouse.sens = s.sens;
				q.device.mouse.poll = s.poll > 0.f ? s.poll : 1000.f;

			} else if (s.kind == input::tablet && s.desktop[a]) {

				// a doubled range is coded in the driver's own integers, over a desktop twice as wide
				const double span{ (double)s.desktop[a] * (s.range == 32767 ? 2. : 1.) };

				q.device.kind = dspace::axis_mode::absolute;
				q.device.absolute.speed = s.sens > 0.f ? s.sens : 1.f;
				q.game.window_half = (float)(s.window[a] / 2);
				q.game.fp = s.x87[a] ? dspace::float_unit::x87 : dspace::float_unit::sse;

				if (s.to_window[a]) {

					// the window stands for the desktop, so a doubled range is coded in the speed
					q.device.absolute.mapping = dspace::tablet_mapping::window;
					q.device.absolute.span = (float)s.window[a];
					q.device.absolute.speed *= s.range == 32767 ? 2.f : 1.f;
					q.game.product_rounded = s.product_rounded[a];

				} else {

					q.device.absolute.mapping = dspace::tablet_mapping::desktop;
					q.device.absolute.span = (float)span;
					q.game.origin = (float)-s.window_at[a];
					q.game.origin_unrounded = s.origin_unrounded[a];

				}

				h.grid_steps[a] = s.grid_step[a];

			} else
				continue;

			h.axis[a] = q;
			h.missed[a] = s.missed[a];

		}

		return h;
	}

	// fitted: the native grid that fits each axis, for those the model was not coded on
	inline dspace::_setup of(const dspace::_coding_model& m, const dspace::_native_grid fitted[2] = nullptr) {

		using dspace::axis_mode;
		using input = dspace::_setup::input;

		dspace::_setup s{};

		s.missed[0] = m.missed[0];
		s.missed[1] = m.missed[1];

		const auto& ax{ m.axes.axis };

		if (ax[0].seg.size() != 1 || ax[1].seg.size() != 1)
			return s;

		const dspace::axis_params* p[2]{ &ax[0].seg[0].p, &ax[1].seg[0].p };

		const auto kind = [&](int a) { return p[a]->device.kind; };

		if (kind(0) == axis_mode::absolute && kind(1) == axis_mode::absolute) {

			s.kind = input::tablet;

			const dspace::_native_grid grid[2]{
				m.grid[0].on || !fitted ? m.grid[0] : fitted[0],
				m.grid[1].on || !fitted ? m.grid[1] : fitted[1]
			};

			_tablet_line line[2];
			double step[2]{}; // coded integers per tablet count
			double other[2]{}; // the area on the complementary lattice

			// per axis: the pixels the coded integers' range covers, where the window starts in them, the speed coded
			double reach[2]{}, at[2]{}, window[2]{};
			float speed[2]{ 1.f, 1.f };

			bool half_range{ true }, full_range{ true }, any_grid{};

			for (int a{}; a < 2; ++a) {

				const auto& d{ p[a]->device.absolute };

				s.x87[a] = p[a]->game.fp == dspace::float_unit::x87;
				s.unrounded_sub[a] = p[a]->game.unrounded_sub;
				s.origin_unrounded[a] = p[a]->game.origin_unrounded;
				s.product_rounded[a] = p[a]->game.product_rounded;

				line[a] = tablet_line(*p[a]);

				// the field mapping names the window; the half size only enters the arithmetic at other speeds
				s.window[a] = window_of_axis(*p[a]);

				if (s.window[a] == 0)
					s.window[a] = (u32)std::lround(2. * p[a]->game.window_half);

				window[a] = s.window[a];

				if (d.mapping == dspace::tablet_mapping::desktop) {

					reach[a] = d.span;
					at[a] = -(double)p[a]->game.origin;
					speed[a] = d.speed;

				} else {

					// the game's own mapping onto the window, at its sensitivity
					reach[a] = d.span;
					speed[a] = d.speed;
					s.to_window[a] = true;

				}

				if (!grid[a].on)
					continue;

				any_grid = true;

				step[a] = s.grid_step[a] = (double)grid[a].A / 4294967296.;

				// The driver's range shows in which makes the area whole: counted over the desktop, or across the window,
				// which holds 65536 / range of the driver's. A 0..32767 range arrives doubled, so 65534 stands for it.
				const double across{ window[a] > 0. ? window[a] / (step[a] * line[a].quantum) : 0. };

				const auto fits = [&](double r) { return whole(r / step[a]) || whole(across * r / 65536.); };

				half_range &= fits(65534.) && !fits(65535.);
				full_range &= fits(65535.) && !fits(65534.);

			}

			// one game has one sensitivity: axes coded at different speeds are each the desktop their speed stands for
			if (speed[0] != speed[1]) {

				for (int a{}; a < 2; ++a) {

					if (speed[a] == 1.f)
						continue;

					reach[a] = line[a].quantum * 65536.;
					at[a] = whole_start(-line[a].at_zero, reach[a]);
					speed[a] = 1.f;
					s.to_window[a] = false;

				}

			}

			s.sens = speed[0];

			// coded at speed 1, the sensitivity may still be in what the integers cover
			if (s.sens == 1.f && !s.to_window[0] && !s.to_window[1]) {

				if (const float sens{ tablet_sensitivity(reach, window) }; sens != 1.f) {

					s.sens = sens;

					for (int a{}; a < 2; ++a) {

						reach[a] = window[a];
						at[a] = 0.;

					}

				}

			}

			// a 0..32767 range arrives as even integers, and the model then codes the driver's own, half as many: the desktop
			// it implies is twice the real one. Where the grids do not show the range, a desktop no pair of monitors spans
			// does, and so does one twice the window both ways: four such monitors in a square are rarer than that driver
			// on one.
			const bool halved_integers{ any_grid && half_range ? true : any_grid && full_range ? false :
				reach[0] > 5200. || reach[1] > 2200. || (reach[0] == 2. * window[0] && reach[1] == 2. * window[1])
			};

			s.range = halved_integers ? 32767 : 65535;

			// No desktop is half its window: where the integers cover the window alone, the doubled range is in the speed,
			// coded at twice the game's sensitivity.
			const bool doubled_speed{ halved_integers && reach[0] == window[0] && reach[1] == window[1] };

			if (doubled_speed)
				s.sens *= 0.5f;

			double desktop[2]{};

			for (int a{}; a < 2; ++a) {

				desktop[a] = reach[a] / (halved_integers && !doubled_speed ? 2. : 1.);

				s.desktop[a] = (u32)std::lround(desktop[a]);

				// shifting every coded integer moves the window by whole pixels only every desktop / gcd(desktop, range)
				// of them, so its position is known up to that period. The coded one is kept: the game's float rounding
				// follows the origin, and only that one is sure to give the replay back.
				s.window_at[a] = (i32)std::llround(at[a]);
				s.window_period[a] = (u32)std::max<i64>(1, (i64)s.desktop[a] / (i64)std::gcd((u64)std::max<u32>(1, s.desktop[a]), (u64)(halved_integers ? 32768 : 65536)));

				if (step[a] > 0. && s.window[a]) {

					// counts over the window = window pixels / pixels per count
					const double counts{ (double)s.window[a] / (step[a] * line[a].quantum) };

					// whole under the driver's own arithmetic: (range or range + 1) counts over what the integers cover
					double exact{ counts };
					bool over_desktop{};

					for (const double r : { 65535., 65534., 65536. }) {

						const double over_range{ r / step[a] };

						if (whole(over_range)) {

							// integers on a lattice of N steps over the range are also on one of range - N steps (each is
							// its own count plus the other's): the tablet's is the coarser of the two, or the one below
							const double n{ std::round(over_range) }, to_window{ (double)s.window[a] / (line[a].quantum * 65536.) };

							exact = std::min(n, r - n) * to_window;
							other[a] = std::max(n, r - n) * to_window;
							over_desktop = true;

							break;
						}

					}

					// An area mapped to one monitor of several is whole over that monitor only. The driver spreads the desktop
					// over its range and the game over 65536, so the game's window holds 65536 / range of the driver's: the
					// area is that much less than the counts across it.
					for (const double r : { 65535., 65534., 65536. }) {

						if (over_desktop || !whole(counts * r / 65536.))
							continue;

						exact = counts * r / 65536.;

						break;
					}

					s.area[a] = whole(exact) ? std::round(exact) : std::round(exact * 10.) / 10.;

				}

			}

			// Which of two complementary lattices is the tablet's shows in the area's shape. Most areas are the window's
			// shape (mapped to keep proportions) or the tablet's own (the whole of it: 16:10, 3:2 or 4:3): the combination
			// within a fiftieth of one of those stands, the nearest. Where none is, the one nearest the window's.
			if (s.area[0] > 0. && s.area[1] > 0. && s.window[0] && s.window[1]) {

				const double shapes[]{ (double)s.window[0] / s.window[1], 16. / 10., 3. / 2., 4. / 3. };

				// how far x by y is from the nearest of the first n shapes
				const auto off_shape = [&shapes](double x, double y, size_t n) {

					double least{ std::numeric_limits<double>::max() };

					for (size_t k{}; k < n; ++k)
						least = std::min(least, std::abs(x / y / shapes[k] - 1.));

					return least;
				};

				const double first[2]{ s.area[0], s.area[1] };

				const bool likely{ [&] {

					for (int k{}; k < 4; ++k)
						if (const double x{ k & 1 ? other[0] : first[0] }, y{ k & 2 ? other[1] : first[1] }; x > 0. && y > 0. && off_shape(x, y, std::size(shapes)) < 0.02)
							return true;

					return false;
				}() };

				const size_t n{ likely ? std::size(shapes) : 1 };

				double best{ off_shape(first[0], first[1], n) };

				for (int k{ 1 }; k < 4; ++k) {

					const double x{ k & 1 ? other[0] : first[0] }, y{ k & 2 ? other[1] : first[1] };

					if (x > 0. && y > 0. && off_shape(x, y, n) < best) {

						best = off_shape(x, y, n);
						s.area[0] = whole(x) ? std::round(x) : std::round(x * 10.) / 10.;
						s.area[1] = whole(y) ? std::round(y) : std::round(y * 10.) / 10.;

					}

				}

			}

			// the axes are fit apart and can settle on different windows: a window 4/3 the size on a desktop 4/3 the size
			// gives the same positions. One window serves both when the other axis's desktop stays whole at its size, and
			// its window still starts on a whole pixel of it.
			const double hx{ (double)p[0]->game.scale * 480. }, hy{ (double)p[1]->game.scale * 480. };

			if (std::abs(hx - hy) > 0.5 && hx > 0. && hy > 0.) {

				const double to_y{ hy / hx };

				const auto on_pixel = [](double at) { return std::abs(at - std::round(at)) < 0.05; };

				if (const double dx{ desktop[0] * to_y }; whole(dx) && whole(s.window[0] * to_y) && on_pixel(s.window_at[0] * to_y)) {

					s.desktop[0] = (u32)std::lround(dx);
					s.window[0] = (u32)std::lround(s.window[0] * to_y);
					s.window_at[0] = (i32)std::lround(s.window_at[0] * to_y);

				} else if (const double dy{ desktop[1] / to_y }; whole(dy) && whole(s.window[1] / to_y) && on_pixel(s.window_at[1] / to_y)) {

					s.desktop[1] = (u32)std::lround(dy);
					s.window[1] = (u32)std::lround(s.window[1] / to_y);
					s.window_at[1] = (i32)std::lround(s.window_at[1] / to_y);

				}

			}

			for (int a{}; a < 2; ++a) {

				// the period is the reconciled desktop's
				s.window_period[a] = (u32)std::max<i64>(1, (i64)s.desktop[a] / (i64)std::gcd((u64)std::max<u32>(1, s.desktop[a]), (u64)(halved_integers ? 32768 : 65536)));

				place_window(s, a);

			}

			return s;
		}

		if (kind(0) == axis_mode::mouse && (kind(1) == axis_mode::mouse || kind(1) == axis_mode::text)) {

			s.kind = input::mouse;
			s.sens = p[0]->device.mouse.sens;
			s.poll = p[0]->device.mouse.poll;
			s.unrounded_sub[0] = p[0]->game.unrounded_sub;
			s.unrounded_sub[1] = kind(1) == axis_mode::mouse ? p[1]->game.unrounded_sub : p[0]->game.unrounded_sub;

			const u32 H{ (u32)std::lround((double)p[0]->game.scale * 480.) };

			s.window[0] = window_of_axis(*p[0]);
			s.window[1] = kind(1) == axis_mode::mouse ? window_of_axis(*p[1]) : H;

			// a sensitivity of 1 moves by whole pixels
			if (s.sens == 1.f)
				s.kind = input::pixels;

			return s;
		}

		if (kind(0) == axis_mode::rational) {

			// a lattice of one pixel: the height is 480 times pixels per field unit
			const double scale{ (double)p[0]->device.rational.D / p[0]->device.rational.a };
			const double H{ scale * 480. };

			if (whole(H)) {

				s.kind = input::pixels;
				s.window[1] = (u32)std::lround(H);

			}

			return s;
		}

		return s;
	}

	struct _ticks {
		float rate; // audio time against real time, 0: too few clean ticks to say
		float spread; // how far a tick's spacing is from the period on average, in audio ms
	};

	// The 60 Hz ticks: the game records a frame on the first update after each 60th of a second of real time, so ticks
	// are 16.67 ms of audio apart at normal speed, 25 with double time, 12.5 with half time. A tick waits for its frame,
	// anywhere up to one frame time, so its spacing spreads around the period by a third of a frame time on average.
	inline _ticks ticks_of(const _osr& r) {

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

		double sum{}, off{};
		size_t count{};

		for (size_t i{ 1 }; i + 1 < n; ++i) {

			if (!clean(i))
				continue;

			sum += (double)r[i].delta;
			++count;
		}

		if (count < 50)
			return {};

		const double period{ sum / (double)count };

		for (size_t i{ 1 }; i + 1 < n; ++i)
			if (clean(i))
				off += std::abs((double)r[i].delta - period);

		return { (float)(period / (1000. / 60.)), (float)(off / (double)count) };
	}

	// The game's frame time, in audio ms, from frames recorded singly (on a key or a combo change, between ticks). They
	// land on the game's own frames, so no gap between two frames is shorter than a frame time, and every longer gap
	// is as common as any other. 0: too few short gaps to say. The answer is a whole millisecond: 1 is 1 or less.
	inline double single_frame_ms(const _osr& r, int burst_max) {

		size_t gaps[33]{}, count{};

		for (const auto& f : r)
			if (f.delta >= 1 && f.delta <= burst_max) {

				++gaps[f.delta];
				++count;
			}

		if (count < 300)
			return 0.;

		// how common a gap is once frames can fall on it: the mean over the longer half
		const int from{ burst_max / 2 + 1 };

		double level{};

		for (int d{ from }; d <= burst_max; ++d)
			level += (double)gaps[d];

		level /= (double)(burst_max - from + 1);

		for (int d{ 1 }; d <= burst_max; ++d)
			if ((double)gaps[d] >= 0.5 * level)
				return (double)d;

		return 0.;
	}

	struct _period {
		double ms; // audio time, 0: the replay does not show it
		bool bound; // the period is at most this
	};

	// The cursor device's period from frames recorded singly. Across a gap shorter than the period the cursor moves
	// only if a report fell inside it, so over many gaps of g ms it moved across g / period of them: the period is the
	// gaps' total length over the times it moved. Only gaps the cursor was travelling through count (it covered
	// ground between the frames either side), and only gaps short of the period, where the share has not reached 1.
	inline _period single_report_ms(const _osr& r, float rate, int burst_max) {

		// the cursor at each time the replay has: frames can share a millisecond, and a report shows on any of them
		struct _point {
			i64 at;
			float x, y;
		};

		std::vector<_point> p;

		p.reserve(r.size());

		{
			i64 at{};

			for (const auto& f : r) {

				at += std::max(0, f.delta);

				if (p.empty() || p.back().at != at)
					p.push_back({ at, f.x, f.y });
				else
					p.back() = { at, f.x, f.y };

			}

		}

		const size_t n{ p.size() };

		size_t gaps[33]{}, moved[33]{};

		// Travelling: at least 0.4 field units per ms of real time between two times that stand clear of the gap, 12 ms
		// of real time or more on either side. Times any closer would mostly show movement when a report fell in the
		// gap itself, which is the thing being counted.
		const i64 clear{ (i64)(12.f * rate) };

		size_t before{}, after{};

		for (size_t i{ 2 }; i + 1 < n; ++i) {

			const i64 gap{ p[i].at - p[i - 1].at };

			while (before + 1 < i - 1 && p[i - 1].at - p[before + 1].at >= clear)
				++before;

			after = std::max(after, i);

			while (after + 1 < n && p[after].at - p[i].at < clear)
				++after;

			if (gap > burst_max || p[i - 1].at - p[before].at < clear || p[after].at - p[i].at < clear)
				continue;

			const double span{ (double)(p[after].at - p[before].at) / (double)rate };

			if (span > 80. || std::hypot(p[after].x - p[before].x, p[after].y - p[before].y) < 0.4 * span)
				continue;

			++gaps[gap];
			moved[gap] += p[i].x != p[i - 1].x || p[i].y != p[i - 1].y;
		}

		// the longest gaps that are still short of the period they give
		_period out{};

		for (int cap{ burst_max }; cap >= 1; --cap) {

			size_t count{}, moves{}, length{};

			for (int g{ 1 }; g <= cap; ++g) {

				count += gaps[g];
				moves += moved[g];
				length += (size_t)g * gaps[g];
			}

			if (count < 150 || moves < 40)
				break;

			const double period{ (double)length / (double)moves };

			if (period >= 1.5 * (double)cap)
				return { period, false };

			// every gap this long was moved across: the period is no longer than the gap
			out = { (double)cap, true };
		}

		return out;
	}

	// What the frames' timing shows of the setup, read into s. play_rate is 0 when the caller has none; grid_ms is the
	// period of the grid the times are coded on, 0 for none.
	//
	// Besides the 60 Hz ticks, the game records every frame while some states last (a slider held with relax). So a
	// replay can have stretches recorded frame by frame: there the deltas are the game's own frame times, and a device
	// that reports less often than the game draws shows its period in when the cursor moves. A key change is recorded
	// on the frame that saw it, so the tapping device's reports show in the times between key changes.
	// Frame times are audio time: real time is that over the rate the ticks give.
	inline void timing(const _osr& r, float play_rate, double grid_ms, dspace::_setup& s) {

		using tapping = dspace::_setup::tapping;

		const size_t n{ r.size() };

		if (n < 16)
			return;

		// the ticks say how fast audio time ran; a known play rate they agree with is exact
		const _ticks ticks{ ticks_of(r) };

		float rate{ ticks.rate };

		// the ticks' mean spacing reads up to a few percent short where other frames fall between them: the grid the
		// times are coded on is their exact period where the two agree
		if (const double on_grid{ grid_ms / (1000. / 60.) }; rate >= 0.92 * on_grid && rate <= 1.015 * on_grid)
			rate = (float)on_grid;

		for (const float known : { play_rate, 1.f, 1.5f, 0.75f })
			if (known > 0.f && std::abs(rate / known - 1.f) < 0.015f) {

				rate = known;

				break;
			}

		s.clock_rate = rate;

		if (!(rate > 0.f))
			rate = play_rate > 0.f ? play_rate : 1.f;

		// frames this close together (10 ms of real time) were not waiting for a tick
		const int burst_max{ (int)(10.f * rate) };

		const auto close = [&](size_t i) { return r[i].delta >= 0 && r[i].delta <= burst_max; };

		// a frame inside a stretch recorded frame by frame: its delta is one frame time. Frames a combo change or a
		// key adds between ticks come close together too, but not this many in a row
		constexpr size_t RUN_MIN{ 16 };

		std::vector<u8> run(n);

		for (size_t i{}; i < n;) {

			size_t end{ i };

			while (end < n && close(end))
				++end;

			if (end - i >= RUN_MIN)
				std::fill(run.begin() + (ptrdiff_t)i + 1, run.begin() + (ptrdiff_t)end - 1, (u8)1);

			i = end + 1;
		}

		double frame_ms{}; // audio time

		{
			size_t frames{}, by_delta[32]{};
			i64 span{};

			for (size_t i{}; i < n; ++i) {

				if (!run[i])
					continue;

				++frames;
				span += r[i].delta;
				++by_delta[std::min(r[i].delta, 31)];
			}

			if (frames >= 300 && span > 0) {

				frame_ms = (double)span / (double)frames;

				s.frame_hz = (float)(1000. * rate / frame_ms);

				// a limiter holds every frame to the two whole milliseconds around its period; above 500 frames a
				// second any game does, the times being whole milliseconds
				const size_t lo{ (size_t)frame_ms };

				s.frame_steady = s.frame_hz < 520.f && (double)(by_delta[lo] + by_delta[lo + 1]) >= 0.95 * (double)frames;
			}

		}

		// without such stretches, frames recorded singly still land on the game's frames
		const bool stretches{ frame_ms > 0. };

		if (!stretches)
			if (const double single{ single_frame_ms(r, burst_max) }; single > 0.) {

				s.frame_hz = (float)(1000. * rate / single);
				s.frame_rough = true;
				s.frame_bound = single <= 1.;
			}

		// the times between cursor movements, inside those stretches
		if (frame_ms > 0.) {

			size_t gaps[64]{}, moves{};
			i64 t{}, moved_at{ -1 };
			float x{ r[0].x }, y{ r[0].y };

			for (size_t i{ 1 }; i < n; ++i) {

				if (r[i].delta > 0)
					t += r[i].delta;

				if (!run[i])
					moved_at = -1;

				if (r[i].x == x && r[i].y == y)
					continue;

				if (run[i]) {

					if (moved_at >= 0 && t - moved_at < 64) {

						++gaps[t - moved_at];
						++moves;
					}

					moved_at = t;
				}

				x = r[i].x;
				y = r[i].y;
			}

			if (moves >= 300) {

				size_t mode{ 1 };

				for (size_t g{ 2 }; g < 64; ++g)
					mode = gaps[g] > gaps[mode] ? g : mode;

				// The period is the mean gap, over the gaps of one report: those short of one and a half periods, the
				// rest being reports the cursor sat still for. A driver can deliver a steady device's reports unevenly
				// (gaps of 3 to 10 ms from a 133 Hz tablet), and only the mean of them all is the device's period.
				// The limit follows the mean until it settles.
				double period{ (double)mode };

				for (int pass{}; pass < 8; ++pass) {

					const size_t lo{ 1 }, hi{ std::min<size_t>(63, (size_t)(1.45 * period)) };

					double sum{}, count{};

					for (size_t g{ lo }; g <= hi; ++g) {

						sum += (double)(g * gaps[g]);
						count += (double)gaps[g];
					}

					if (!(count > 0.) || sum / count == period)
						break;

					period = sum / count;
				}

				// the finest step the replay can show: a frame, and no less than a millisecond
				const double finest{ std::max(frame_ms, 1.) };

				if (gaps[0] > gaps[mode] || period <= 1.6 * finest) {

					s.report_hz = (float)(1000. * rate / finest);
					s.report_bound = true;

				} else
					s.report_hz = (float)(1000. * rate / period);

			}

		}

		if (!(s.report_hz > 0.f))
			if (const _period single{ single_report_ms(r, rate, burst_max) }; single.ms > 0.) {

				s.report_hz = (float)(1000. * rate / single.ms);
				s.report_rough = true;
				s.report_bound = single.bound;
			}

		// taps: which buttons go down, and the times between changes
		{
			size_t presses[2]{}; // keys, buttons
			size_t gaps{}, on8{}, on4{};
			i64 t{}, changed_at{ -1 };
			u32 keys{};

			// 8 and 4 ms of real time, whole in audio time at every play rate
			const i64 p8{ std::lround(8.f * rate) }, p4{ std::lround(4.f * rate) };
			const i64 gap_max{ (i64)(400.f * rate) };

			for (size_t i{}; i < n; ++i) {

				if (r[i].delta > 0)
					t += r[i].delta;

				const u32 now{ r[i].keys & 15 };

				if (now == keys)
					continue;

				// a key sets its own bit and its button's
				if (const u32 down{ now & ~keys }; down & 12)
					++presses[0];
				else if (down & 3)
					++presses[1];

				if (const i64 gap{ t - changed_at }; changed_at >= 0 && gap > 0 && gap <= gap_max) {

					++gaps;
					on8 += gap % p8 == 0;
					on4 += gap % p4 == 0;
				}

				changed_at = t;
				keys = now;
			}

			if (const size_t all{ presses[0] + presses[1] }; all >= 20)
				s.taps = presses[0] * 20 >= all * 19 ? tapping::keys : presses[1] * 20 >= all * 19 ? tapping::buttons : tapping::both;

			// A device's steps show when the game's frames are at most half as long. The frame rate says so where the
			// replay has it; failing that the ticks' spread, which is no finer than a frame time, can only rule a test out
			const float frame{ s.frame_hz > 0.f ? 1000.f / s.frame_hz : 3.f * ticks.spread / rate };
			const bool known{ stretches };

			if (gaps >= 100 && (known || ticks.rate > 0.f)) {

				if (frame <= 4.f && on8 * 5 >= gaps * 2)
					s.tap_hz = 125.f;
				else if (frame <= 2.f && on4 * 5 >= gaps * 3)
					s.tap_hz = 250.f;
				else if (known && frame <= 4.f) {

					// no steps seen: it reports at least twice as often as the coarsest step tested
					s.tap_hz = frame <= 2.f ? 500.f : 250.f;
					s.tap_bound = true;
				}

			}

		}

	}

}

}
