#pragma once

#include <span>

namespace orz {

	namespace orz_text {

		template <bool is_mania>
		inline std::string minify_key_frames_text(const std::vector<_osr_frame>& R) {

			std::string out;

			if (R.empty())
				return out;

			out.reserve(R.size() * 24);

			char b[64];

			const auto get_state = [](const _osr_frame& r) -> u32 {

				return is_mania ? u32(r.x) : r.keys;
			};

			const auto write_frame = [&](const i64 delta, const u32 state, float y) {

				if (out.size())
					out += ',';

				char* p{ std::to_chars(b, b + sizeof b, delta).ptr };

				if constexpr (is_mania) {

					*p++ = '|';
					p = std::to_chars(p, b + sizeof b, state).ptr;

					*p++ = '|';
					p = dotnet::to_string(y, p, 0);

					*p++ = '|';
					p = std::to_chars(p, b + sizeof b, state).ptr;

				} else {

					memcpy(p, "|0|0|", 5);
					p += 5;

					p = std::to_chars(p, b + sizeof b, state).ptr;

				}

				out.append(b, p);

			};

			i64 merge_delta{};
			u32 last_state{ ~get_state(R[0]) };

			for (const auto& r : R) {

				merge_delta += i64(r.delta);

				const u32 state{ get_state(r) };

				if (last_state == state)
					continue;

				write_frame(merge_delta, state, r.y);

				merge_delta = 0;
				last_state = state;

			}

			return out;
		}

		inline std::string minify_std_frames_text(const i64 rate, const std::vector<_osr_frame>& R) {

			std::string out;

			out.reserve(R.size() * 24);

			char b[64];

			const auto write_frame = [&](const i64 delta, const auto x, const auto y, const u32 keys, const bool key_frame) {

				if (out.size())
					out += ',';

				char* p{ std::to_chars(b, b + sizeof b, delta).ptr };

				*p++ = '|';

				if (key_frame) {
					p = dotnet::to_string(x, p, 0);
					*p++ = '|';
					p = dotnet::to_string(y, p, 0);
				}
				else {
					p = std::to_chars(p, b + sizeof(b), i32(x + 0.5f)).ptr;
					*p++ = '|';
					p = std::to_chars(p, b + sizeof(b), i32(y + 0.5f)).ptr;
				}

				*p++ = '|';
				p = keys ? std::to_chars(p, b + sizeof b, keys).ptr : (*p = '0', p + 1);

				out.append(b, p);

			};

			if (R.size() < 3 || rate <= 1) {

				for (const auto& r : R)
					write_frame(r.delta, r.x, r.y, r.keys, true);

				return out;
			}

			write_frame(R[0].delta, R[0].x, R[0].y, R[0].keys, true);
			write_frame(R[1].delta, R[1].x, R[1].y, R[1].keys, true);

			i64 running_time{ i64(R[0].delta) + i64(R[1].delta) };
			i64 output_time{ running_time };
			i64 next_sample_time{ running_time + rate };

			const _osr_frame* previous{ &R[1] };
			i64 previous_time{ running_time };

			u32 last_keys{ R[1].keys };
			bool first_gameplay_frame{ true };

			const auto fill_lerp_frames = [&](const _osr_frame& current, const bool include_endpoint) {

				const i64 span{ running_time - previous_time };

				if (span <= 0 || span > 50)
					return;

				const float inv_span{ 1.f / static_cast<float>(span) };

				while (next_sample_time < running_time || (include_endpoint && next_sample_time == running_time)) {

					const i64 sample_time{ next_sample_time };

					const float t{ static_cast<float>(sample_time - previous_time) * inv_span };

					const float x{ previous->x + (current.x - previous->x) * t };
					const float y{ previous->y + (current.y - previous->y) * t };

					write_frame(rate, x, y, 0, false);

					output_time = sample_time;
					next_sample_time += rate;
				}

			};

			for (size_t i{ 2 }; i + 1 < R.size(); ++i) {

				const auto& r{ R[i] };

				running_time += i64(r.delta);

				const bool key_frame{ first_gameplay_frame || r.keys != 0 || r.keys != last_keys };

				first_gameplay_frame = false;
				last_keys = r.keys;

				if (key_frame || r.delta > 50 || r.delta < 0) {

					write_frame(running_time - output_time, r.x, r.y, r.keys, key_frame);

					output_time = running_time;
					next_sample_time = running_time + rate;

				} else {

					fill_lerp_frames(r, true);

				}

				previous = &r;
				previous_time = running_time;
			}

			{

				const auto& tail{ R.back() };

				write_frame(tail.delta, tail.x, tail.y, tail.keys, true);

			}

			return out;
		}

	}

}