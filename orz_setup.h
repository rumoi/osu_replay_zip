#pragma once

// the setup behind an osu!standard coding model. Several models give the same positions: a tablet mapped to the window
// at speed 2 is one mapped to a desktop twice as wide, a driver with half the output range doubles every speed. What
// they share is where a tablet count lands in the window, so the setup is stated in those terms.

#include <cmath>
#include <numeric>

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

	// the output ranges a tablet driver spreads its counts over, the likeliest first: 65535, a doubled 0..32767, 65536
	constexpr double DRIVER_RANGES[]{ 65535., 65534., 65536. };

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

				// the window's position as coded: the game's float rounding follows the origin, and only that one is sure to
				// give the replay back
				s.window_at[a] = (i32)std::llround(at[a]);

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
	};

	// The 60 Hz ticks: the game records a frame on the first update after each 60th of a second of real time, so ticks
	// are 16.67 ms of audio apart at normal speed, 25 with double time, 12.5 with half time.
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

		double sum{};
		size_t count{};

		for (size_t i{ 1 }; i + 1 < n; ++i) {

			if (!clean(i))
				continue;

			sum += (double)r[i].delta;
			++count;
		}

		if (count < 50)
			return {};

		return { (float)(sum / (double)count / (1000. / 60.)) };
	}

	// How fast audio time ran against real time, 0 where the replay does not show it. play_rate is the rate the replay's
	// mods state (0: unknown); grid_ms is the period of the grid the times are coded on, 0 for none.
	inline float clock_rate(const _osr& r, float play_rate, double grid_ms) {

		float rate{ ticks_of(r).rate };

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
