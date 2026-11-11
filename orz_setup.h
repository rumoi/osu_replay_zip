#pragma once

// the setup behind an osu!standard coding model: what its two axes say together of the window, the device and the
// game's arithmetic, in the terms a caller keeps and hands back as a hint for the player's next replay

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

	// The device a setup stands for on an axis, kind none where it has none: its display, driver and client as the
	// axis's device. A setup handed back as a hint is taken apart this way: a device that no longer fits is searched
	// again, and the tablet's lattice goes by its step (grid_step) and is fit again.
	inline dspace::axis_params device(const dspace::_setup& s, int a) {

		using input = dspace::_setup::input;

		dspace::axis_params q{};

		// whole pixels: the lattice of one pixel in field units, offset by where the playfield starts. Only the height
		// is known, and the width only matters through being even, as nearly every window's is.
		if (s.kind == input::pixels && s.window[1]) {

			const int H{ (int)s.window[1] };
			const int g{ (int)std::gcd(H, 480) };
			const int D{ H / g }, A{ 480 / g };

			const auto field{ dspace::playfield(s.window[0] ? (int)s.window[0] : 2 * H, H) };

			// value = (pixel - off) * A / D, so b = -A * off, kept within one step
			const i64 b{ (i64)std::llround(-(double)A * (double)(a ? field.off_y : field.off_x)) };

			q.device.kind = dspace::axis_mode::rational;
			q.device.rational.D = D;
			q.device.rational.a = A;
			q.device.rational.b = (int)(((b % A) + A) % A);

			return q;
		}

		if (!s.window[0] || !s.window[1])
			return q;

		if (s.kind == input::mouse) {

			q.device.kind = dspace::axis_mode::mouse;
			q.device.mouse.sens = s.sens;
			q.device.mouse.poll = s.poll > 0.f ? s.poll : 1000.f;

		} else if (s.kind == input::tablet && s.desktop[a]) {

			q.device.kind = dspace::axis_mode::absolute;
			q.device.absolute.speed = s.sens > 0.f ? s.sens : 1.f;
			q.game.window_half = (float)(s.window[a] / 2);
			q.game.fp = s.x87[a] ? dspace::float_unit::x87 : dspace::float_unit::sse;

			if (s.to_window[a]) {

				q.device.absolute.mapping = dspace::tablet_mapping::window;
				q.device.absolute.span = (float)s.window[a];
				q.game.product_rounded = s.product_rounded[a];

			} else {

				q.device.absolute.mapping = dspace::tablet_mapping::desktop;
				q.device.absolute.span = (float)s.desktop[a];
				q.game.origin = (float)-s.window_at[a];
				q.game.origin_unrounded = s.origin_unrounded[a];

			}

		} else
			return q;

		const auto field{ dspace::playfield((int)s.window[0], (int)s.window[1]) };

		q.game.scale = field.ratio;
		q.game.off = a ? field.off_y : field.off_x;
		q.game.unrounded_sub = s.unrounded_sub[a];

		return q;
	}

	// grid: the native grid that fits each axis, coded on or not. missed: the share of each axis's positions the model
	// did not give.
	inline dspace::_setup of(const dspace::_model_info& m, const dspace::_native_grid grid[2], const float missed[2]) {

		using dspace::axis_mode;
		using input = dspace::_setup::input;

		dspace::_setup s{};

		s.missed[0] = missed[0];
		s.missed[1] = missed[1];

		const auto& ax{ m.axis };

		if (ax[0].seg.size() != 1 || ax[1].seg.size() != 1)
			return s;

		const dspace::axis_params* p[2]{ &ax[0].seg[0].p, &ax[1].seg[0].p };

		const auto kind = [&](int a) { return p[a]->device.kind; };

		if (kind(0) == axis_mode::absolute && kind(1) == axis_mode::absolute) {

			s.kind = input::tablet;

			// per axis: the pixels the coded integers' range covers, where the window starts in them, the speed coded
			double reach[2]{}, at[2]{};
			float speed[2]{ 1.f, 1.f };

			for (int a{}; a < 2; ++a) {

				const auto& d{ p[a]->device.absolute };

				s.x87[a] = p[a]->game.fp == dspace::float_unit::x87;
				s.unrounded_sub[a] = p[a]->game.unrounded_sub;
				s.origin_unrounded[a] = p[a]->game.origin_unrounded;
				s.product_rounded[a] = p[a]->game.product_rounded;

				// the field mapping names the window; the half size only enters the arithmetic at other speeds
				s.window[a] = window_of_axis(*p[a]);

				if (s.window[a] == 0)
					s.window[a] = (u32)std::lround(2. * p[a]->game.window_half);

				reach[a] = d.span;
				speed[a] = d.speed;

				// through the desktop, or the game's own mapping onto the window
				if (d.mapping == dspace::tablet_mapping::desktop)
					at[a] = -(double)p[a]->game.origin;
				else
					s.to_window[a] = true;

				if (grid[a].on)
					s.grid_step[a] = (double)grid[a].A / 4294967296.;

			}

			// one game has one sensitivity: axes coded at different speeds are each the desktop their speed stands for
			if (speed[0] != speed[1]) {

				for (int a{}; a < 2; ++a) {

					if (speed[a] == 1.f)
						continue;

					const _tablet_line line{ tablet_line(*p[a]) };

					reach[a] = line.quantum * 65536.;
					at[a] = whole_start(-line.at_zero, reach[a]);
					speed[a] = 1.f;
					s.to_window[a] = false;

				}

			}

			s.sens = speed[0];

			for (int a{}; a < 2; ++a) {

				s.desktop[a] = (u32)std::lround(reach[a]);

				// the window's position as coded: the game's float rounding follows the origin, and only that one is sure to
				// give the replay back
				s.window_at[a] = (i32)std::llround(at[a]);

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

}

}
