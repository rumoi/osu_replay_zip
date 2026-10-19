#pragma once

#include <charconv>
#include <string>
#include <vector>

#include "orz_dotnet.h"

namespace orz {

namespace orz_text {

	inline std::string frames_text(const std::vector<_osr_frame>& R, bool exact_text) {

		std::string out;

		out.reserve(R.size() * 24);

		char b[64];

		for (size_t f{}; f < R.size(); ++f) {

			const auto& r{ R[f] };

			if (f)
				out += ',';

			char* p{ std::to_chars(b, b + sizeof b, r.delta).ptr };

			*p++ = '|';
			p = dotnet::to_string(r.x, p, exact_text);
			*p++ = '|';
			p = dotnet::to_string(r.y, p, exact_text);
			*p++ = '|';
			p = std::to_chars(p, b + sizeof b, r.keys).ptr;

			out.append(b, p);

		}

		return out;
	}

	inline std::vector<_osr_frame> parse_frames(const char* data, size_t size) {

		std::vector<_osr_frame> out;

		out.reserve(size / 20);

		const char* frame_start{ data };
		const char* const end_all{ data + size };

		const auto add_frame = [&](const char* end) {

			if (frame_start == end)
				return;

			_osr_frame f{};

			const char* sep[3]{ end, end, end };
			size_t c{};

			for (const char* p{ frame_start }; p < end && c < 3; ++p)
				if (*p == '|')
					sep[c++] = p;

			std::from_chars(frame_start, sep[0], f.delta);

			if (c >= 1)
				std::from_chars(sep[0] + 1, sep[1], f.x);

			if (c >= 2)
				std::from_chars(sep[1] + 1, sep[2], f.y);

			if (c >= 3)
				std::from_chars(sep[2] + 1, end, f.keys);

			out.push_back(f);

			frame_start = end + 1;

		};

		for (const char* p{ data }; p < end_all; ++p)
			if (*p == ',')
				add_frame(p);

		add_frame(end_all);

		return out;
	}

}

}
