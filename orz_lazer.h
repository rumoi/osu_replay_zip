#pragma once

// osu!lazer's whole pixel positions, found from a replay alone: the window whose playfield mapping gives the replay's
// positions back as lazer's float arithmetic makes them (chain_params, device_space.h)

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

#include "device_space.h"

namespace orz {

namespace lazer {

	// The step between neighbouring positions of an axis, 0 when it shows none: the smallest of the three commonest
	// gaps between its sorted values, as the median of the gaps beside it.
	inline double step_of(const std::vector<float>& sorted) {

		std::map<int, size_t> bins;
		std::vector<double> gaps;

		for (size_t i{ 1 }; i < sorted.size(); ++i)
			if (const double g{ (double)sorted[i] - (double)sorted[i - 1] }; g > 1e-3 && g < 4.) {

				gaps.push_back(g);
				++bins[(int)std::lround(g * 1000.)];

			}

		if (gaps.size() < 64)
			return 0.;

		std::vector<std::pair<size_t, int>> common;

		for (const auto& [bin, n] : bins)
			common.push_back({ n, bin });

		std::sort(common.rbegin(), common.rend());
		common.resize(std::min<size_t>(common.size(), 3));

		const double near{ std::min_element(common.begin(), common.end(), [](const auto& a, const auto& b) { return a.second < b.second; })->second / 1000. };

		gaps.erase(std::remove_if(gaps.begin(), gaps.end(), [&](double g) { return std::abs(g - near) > 0.002; }), gaps.end());

		if (gaps.empty())
			return 0.;

		std::nth_element(gaps.begin(), gaps.begin() + gaps.size() / 2, gaps.end());

		return gaps[gaps.size() / 2];
	}

	// the floats from n below x to n above it
	inline std::vector<float> around(float x, int n) {

		std::vector<float> r{ x };

		for (float lo{ x }, hi{ x }; n > 0; --n) {

			r.push_back(lo = std::nextafter(lo, -INFINITY));
			r.push_back(hi = std::nextafter(hi, INFINITY));

		}

		return r;
	}

	// the share of the values the device gives exactly
	inline double hits(const dspace::axis_params& q, const std::vector<float>& vals) {

		size_t n{};

		for (const float x : vals)
			n += dspace::rational_value(q, dspace::rational_k(q, x)) == x;

		return vals.empty() ? 0. : (double)n / (double)vals.size();
	}

	// One axis as whole pixels of a window, by lazer's playfield: the window's height from the step between positions,
	// then the floats of the inverse draw matrix beside what the layout says, kept when they give 97% of the positions
	// exactly. The width only moves x's offset, and is searched. H is the window's height: given for x (0 for y, which
	// finds it), and set to the one found.
	inline bool detect_axis(const std::vector<float>& vals, int axis, dspace::axis_params& out, int& H) {

		std::vector<float> sorted;

		for (const float x : vals)
			if (std::abs(x) < 5000.f)
				sorted.push_back(x);

		std::sort(sorted.begin(), sorted.end());
		sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());

		const double step{ step_of(sorted) };

		if (!(step > 0.))
			return false;

		std::vector<float> few;

		for (size_t i{}; i < sorted.size(); i += std::max<size_t>(1, sorted.size() / 96))
			few.push_back(sorted[i]);

		dspace::axis_params best{};
		double best_hits{};

		const auto try_window = [&](int w, int h) {

			const double scale{ h / 480. };

			// the playfield is 512 wide in the middle of the window, and starts 56 above the top of a 480 tall one
			const double off{ axis ? -56. : -((double)w / scale - 512.) / 2. };

			// a pixel is a whole number of steps from the offset
			if (const double at{ ((double)few[few.size() / 2] - off) / (480. / h) }; std::abs(at - std::round(at)) > 0.05)
				return;

			dspace::axis_params q{};

			q.device.kind = dspace::axis_mode::rational;
			q.device.rational.chain.A = 1.f;

			for (const float B : around((float)(480. / h), 3))
				for (const float C : around((float)off, 8)) {

					q.device.rational.chain.B = B;
					q.device.rational.chain.C = C;

					if (const double n{ hits(q, few) }; n > best_hits) {

						best_hits = n;
						best = q;

					}

				}

		};

		const int from{ H ? H : (int)std::lround(480. / step) - 1 }, to{ H ? H : from + 2 };

		for (int h{ from }; h <= to && best_hits < 0.99; ++h) {

			if (h < 240 || h > 4320)
				continue;

			if (axis) {

				try_window(0, h);

				continue;
			}

			// 4:3 to 21:9, the listed windows of this height first
			for (const auto& [lw, lh] : dspace::PM_RES)
				if (lh == h && best_hits < 0.99)
					try_window(lw, h);

			for (int w{ h * 4 / 3 }; w <= h * 12 / 5 && best_hits < 0.99; ++w)
				try_window(w, h);

		}

		if (best_hits < 0.9 || hits(best, vals) < 0.97)
			return false;

		out = best;
		H = (int)std::lround(480. / (double)best.device.rational.chain.B);

		return true;
	}

	// both axes, y first: its offset does not depend on the window's width, and it gives x the height
	inline bool detect(const std::vector<float> vals[2], dspace::_model_info& info) {

		dspace::axis_params p[2]{};
		int H{};

		if (!detect_axis(vals[1], 1, p[1], H) || !detect_axis(vals[0], 0, p[0], H))
			return false;

		info.axis[0] = dspace::axis_model{ p[0] };
		info.axis[1] = dspace::axis_model{ p[1] };

		return true;
	}

}

}
