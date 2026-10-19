#pragma once

#include <charconv>
#include <span>
#include <string>
#include <vector>

#include "orz_shared.h"
#include "rc.h"
#include "orz_dotnet.h"
#include "life_prior.h"

namespace orz {

namespace life {

	enum format : u8 {
		format_text = 0, // the original string
		format_compact = 1
	};

	struct point {
		i64 time;
		int hundredths;
	};

	// the game records a point once more than 2000 ms have passed since the previous one
	constexpr i64 GAP{ 2000 };

	inline std::string to_text(const std::vector<point>& pts) {

		std::string out;

		out.reserve(pts.size() * 10);

		char b[32];

		for (const auto& p : pts) {

			out.append(b, std::to_chars(b, b + sizeof(b), p.time).ptr);
			out += '|';
			dotnet::append_hundredths(out, p.hundredths);
			out += ',';

		}

		return out;
	}

	// false for anything out of range or not a "time|life," list, which stays text
	inline bool parse(const std::string& text, std::vector<point>& pts) {

		pts.clear();
		pts.reserve(text.size() / 6);

		size_t i{};

		while (i < text.size()) {

			const size_t pipe{ text.find('|', i) };
			const size_t comma{ text.find(',', i) };

			if (pipe == std::string::npos || (comma != std::string::npos && comma < pipe))
				return false;

			const size_t end{ comma == std::string::npos ? text.size() : comma };

			point p{};

			if (std::from_chars(text.data() + i, text.data() + pipe, p.time).ptr != text.data() + pipe)
				return false;

			double v{};

			if (std::from_chars(text.data() + pipe + 1, text.data() + end, v).ptr != text.data() + end || !(v >= 0.0 && v <= 1.0))
				return false;

			p.hundredths = (int)std::lround(v * 100.0);

			pts.push_back(p);

			i = end + 1;

		}

		return true;
	}

	FORCEINLINE std::vector<i64> frame_times(const std::span<const _osr_frame> frames) {

		std::vector<i64> t(frames.size());

		i64 total{};

		for (size_t i{}; i < frames.size(); ++i)
			t[i] = (total += frames[i].delta);

		return t;
	}

	constexpr u32 FRAME_INC{ 16 }, VALUE_INC{ 32 }, OTHER_INC{ 20 };
	constexpr int VALUE_EXACT{ 128 };
	constexpr size_t MAX_POINTS{ 1 << 20 };

	static_assert(rc::safe_increment(16, FRAME_INC, LIFE_PRIOR_WEIGHT) && rc::safe_increment(VALUE_EXACT, VALUE_INC, LIFE_PRIOR_WEIGHT) &&
		rc::safe_increment(16, OTHER_INC, 0), "life coder increment / prior weight can overflow the 16 bit frequencies");

	struct _frame_cursor {

		std::span<const i64> times;
		size_t prev_frame{};
		i64 prev_time{};

		size_t start(size_t k, size_t count) const {

			if (k == 0)
				return 0;

			if (k + 1 == count)
				return prev_frame;

			size_t s{ prev_frame };

			while (s < times.size() && times[s] <= prev_time + GAP)
				++s;

			return s;
		}

		static u64 cls(size_t k, size_t count) {

			return k == 0 ? 0 :
				(k + 1 == count ? 2 : 1);
		}

	};

	struct models {

		rc::value_coder count{ 16, OTHER_INC }, frame{ 16, FRAME_INC }, escape{ 16, OTHER_INC }, value{ VALUE_EXACT, VALUE_INC };

		models() {

			frame.prior = { LIFE_FRAME_PRIOR_ROW.data(), LIFE_FRAME_PRIOR_START, LIFE_FRAME_PRIOR_VALUES, LIFE_FRAME_PRIOR_ROWS };
			frame.prior_weight = LIFE_PRIOR_WEIGHT;

			value.prior = { LIFE_VALUE_PRIOR_ROW.data(), LIFE_VALUE_PRIOR_START, LIFE_VALUE_PRIOR_VALUES, LIFE_VALUE_PRIOR_ROWS };
			value.prior_weight = LIFE_PRIOR_WEIGHT;

		}

	};

	inline std::vector<u8> encode_compact(const std::vector<point>& pts, std::span<const i64> times) {

		rc::rc_encoder e{};
		models m{};

		_frame_cursor cursor{ times };

		int prev_value{ 100 };

		m.count.encode(e, 0, pts.size());

		for (size_t k{}; k < pts.size(); ++k) {

			const auto& p{ pts[k] };
			const size_t s{ cursor.start(k, pts.size()) };

			size_t f{ s };

			while (f < times.size() && times[f] != p.time)
				++f;

			if (f < times.size()) {

				m.frame.encode(e, _frame_cursor::cls(k, pts.size()), f - s + 1);
				cursor.prev_frame = f;

			} else {

				m.frame.encode(e, _frame_cursor::cls(k, pts.size()), 0);
				m.escape.encode(e, _frame_cursor::cls(k, pts.size()), rc::zz(p.time - cursor.prev_time));

			}

			cursor.prev_time = p.time;

			m.value.encode(e, (u64)prev_value / 10, (u64)p.hundredths);

			prev_value = p.hundredths;

		}

		return std::move(e.finish());
	}

	inline std::vector<point> decode_compact(std::span<const u8> data, std::span<const i64> times) {

		rc::rc_decoder d{ data };

		models m{};
		_frame_cursor cursor{ times };

		i64 prev_value{ 100 };

		const size_t count{ std::min<size_t>(m.count.decode(d, 0), MAX_POINTS) };

		std::vector<point> pts(count);

		for (size_t k{}; k < count; ++k) {

			auto& p{ pts[k] };

			const size_t s{ cursor.start(k, count) };
			const u64 r{ m.frame.decode(d, _frame_cursor::cls(k, count)) };

			if (r) {

				const size_t f{ s + (size_t)std::min<u64>(r - 1, times.size()) };

				if (f < times.size()) {

					p.time = times[f];
					cursor.prev_frame = f;

				} else
					p.time = cursor.prev_time; // the CRC will fail, damaged file

			} else
				p.time = cursor.prev_time + std::clamp<i64>(rc::unzz(m.escape.decode(d, _frame_cursor::cls(k, count))), -(1ll << 40), 1ll << 40);

			cursor.prev_time = p.time;

			prev_value = std::clamp<i64>((i64)m.value.decode(d, (u64)std::clamp<i64>(prev_value, 0, 100) / 10), 0, 100);

			p.hundredths = (int)prev_value;

		}

		return pts;
	}

	inline std::vector<u8> encode(const std::string& text, std::span<const _osr_frame> frames) {

		std::vector<point> pts;

		if (parse(text, pts) && pts.size() <= MAX_POINTS) {

			const auto times{ frame_times(frames) };
			const auto coded{ encode_compact(pts, times) };

			// empty/small graphs are smaller as text
			if (coded.size() < text.size() && to_text(decode_compact(coded, times)) == text) {

				std::vector<u8> out{ format_compact };

				out.insert(out.end(), coded.begin(), coded.end());

				return out;
			}

		}

		std::vector<u8> out{ format_text };

		out.insert(out.end(), text.begin(), text.end());

		return out;
	}

	inline std::string decode(const std::vector<u8>& data, std::span<const _osr_frame> frames) {

		if (data.empty())
			return {};

		if (data[0] == format_text)
			return std::string(data.begin() + 1, data.end());

		return to_text(decode_compact(std::span{ data }.subspan(1), frame_times(frames)));
	}

}

}
