#pragma once
namespace orz {

	namespace orz_text {

		std::string minify_frames_text(const i64 rate, const std::vector<_osr_frame>& R) {

			std::string out;

			if (R.empty())
				return out;

			out.reserve(R.size() * 24);

			char b[64];

			const auto write_frame = [&](const i64 delta, const auto x, const auto y, const u32 keys, const bool key_frame) {

				if (out.empty())
					out += ',';

				char* p{ std::to_chars(b, b + sizeof b, delta).ptr };

				*p++ = '|';

				if (key_frame) {

					p = dotnet::to_string(x, p, 0);

					*p++ = '|';

					p = dotnet::to_string(y, p, 0);

				}
				else {

					p = std::to_chars(p, b + sizeof(b), static_cast<i32>(x + 0.5f)).ptr;

					*p++ = '|';

					p = std::to_chars(p, b + sizeof(b), static_cast<i32>(y + 0.5f)).ptr;

				}

				*p++ = '|';

				p = std::to_chars(p, b + sizeof b, keys).ptr;

				out.append(b, p);

			};

			if (rate == 0) {

				for (const auto& r : R)
					write_frame(r.delta, r.x, r.y, r.keys, true);

				return out;
			}

			bool previous_was_low{};
			bool last_key_frame{ 1 };

			i64 running_time{};
			i64 output_time{};
			i64 next_sample_time{};

			const _osr_frame* previous{ R.data() };
			i64 previous_time{};

			const auto fill_lerp_frames = [&](const _osr_frame& current, const bool include_endpoint) {

				const i64 span{ running_time - previous_time };

				if (span <= 0 || span > 50)
					return;

				const float inv_span{ 1.f / static_cast<float>(span) }; // could be a table index instead

				while (next_sample_time < running_time || (include_endpoint && next_sample_time == running_time)) {

					const i64 sample_time{ next_sample_time };

					const float t{ static_cast<float>(sample_time - previous_time) * inv_span };

					const float sample_x{ previous->x + (current.x - previous->x) * t };
					const float sample_y{ previous->y + (current.y - previous->y) * t };

					write_frame(rate, sample_x, sample_y, 0, false);

					output_time = sample_time;
					next_sample_time += rate;

				}

			};

			for (const auto& r : R) {

				running_time += i64(r.delta);

				const bool key_frame{ r.keys != 0 };
				const bool important{ key_frame || last_key_frame };
				const bool long_frame{ r.delta > 50 };

				if (important) {

					if (key_frame && previous_was_low)
						fill_lerp_frames(r, false);

					write_frame(running_time - output_time, r.x, r.y, r.keys, true);

					output_time = running_time;
					next_sample_time = running_time + rate;

				} else if (long_frame) {

					write_frame(running_time - output_time, r.x, r.y, 0, false);

					output_time = running_time;
					next_sample_time = running_time + rate;

				} else {

					fill_lerp_frames(r, true);

				}

				previous = &r;
				previous_time = running_time;

				previous_was_low = !important;
				last_key_frame = key_frame;

			}

			return out;
		}

	}

}