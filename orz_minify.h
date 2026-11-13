#pragma once
namespace orz {

	namespace orz_text {

		inline std::string minify_frames_text(const i64 rate, const std::vector<_osr_frame>& R) {

			std::string out;

			if (R.empty())
				return out;

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

				} else {

					p = std::to_chars(p, b + sizeof(b), static_cast<i32>(x + 0.5f)).ptr;

					*p++ = '|';

					p = std::to_chars(p, b + sizeof(b), static_cast<i32>(y + 0.5f)).ptr;

				}

				*p++ = '|';

				p = keys == 0 ? (*p = '0', ++p) : std::to_chars(p, b + sizeof b, keys).ptr;

				out.append(b, p);

			};

			if (rate <= 1) {

				for (const auto& r : R)
					write_frame(r.delta, r.x, r.y, r.keys, true);

				return out;
			}

			u32 last_keys{~R[0].keys};

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

				const bool key_frame{ r.keys || (r.keys != last_keys) };

				last_keys = r.keys;

				if (key_frame || r.delta > 50) {

					write_frame(running_time - output_time, r.x, r.y, r.keys, key_frame);

					output_time = running_time;
					next_sample_time = running_time + rate;

				} else {

					fill_lerp_frames(r, true);

				}

				previous = &r;
				previous_time = running_time;

			}

			return out;
		}

	}

}