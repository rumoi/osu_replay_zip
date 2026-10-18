#define _CRT_SECURE_NO_WARNINGS

#include "orz_shared.h"

#include "orz_write.h"
#include "orz_read.h"

#include <filesystem>

static int run(const int param_count, char** param) {

	const auto usage = []() {

		fprintf(stderr, "usage: orz.exe input.osr|input.orz [-o output]\n");

		return 1;
	};

	std::filesystem::path in, out;

	for (int i{ 1 }; i < param_count; ++i) {

		const std::string_view a{ param[i] };

		if (a == "-o") {

			if (i + 1 >= param_count || !out.empty())
				return usage();

			out = param[++i];

		} else if (in.empty())
			in = param[i];
		else
			return usage();

	}

	if (in.empty())
		return usage();

	const std::string in_s{ in.string() };
	const auto data{ read_file(in_s.c_str()) };

	if (data.empty()) {

		fprintf(stderr, "error: can not read %s\n", in_s.c_str());

		return 1;
	}

	const bool decode{ is_orz(data) };

	if (out.empty())
		out = std::filesystem::path{ in }.replace_extension(decode ? ".osr" : ".orz");

	if (std::filesystem::exists(out) && std::filesystem::equivalent(in, out)) {

		fprintf(stderr, "error: the output would overwrite %s, use -o\n", in_s.c_str());

		return 1;
	}

	const std::string out_s{ out.string() };

	if (decode) {

		if (!orz_to_osr(in_s.c_str(), out_s.c_str())) {

			if (read_container(data).newer)
				fprintf(stderr, "error: %s was made by a newer version of orz\n", in_s.c_str());
			else if (_decoded_orz d; orz_decode_frames(data, d, DECODE_SKIP_CRC))
				fprintf(stderr, "error: %s fails its CRC check (damaged, or written by an incompatible orz build)\n", in_s.c_str());
			else
				fprintf(stderr, "error: %s is not a valid .orz file\n", in_s.c_str());

			return 1;
		}

	} else {

		orz_state st{};

		osr_to_orz(in_s.c_str(), out_s.c_str(), 0, st);

		if (st.output.empty()) {

			fprintf(stderr, "error: %s: %s\n", in_s.c_str(), st.error);

			return 1;
		}

		printf("%s -> %s (reduction %ld%%)\n", in_s.c_str(), out_s.c_str(),
			std::lround(100.0 - 100.0 * st.output.size() / data.size()));

	}

	return 0;
}

int main(const int param_count, char** param) {

	try {
		return run(param_count, param);
	} catch (const std::exception& e) {

		fprintf(stderr, "error: %s\n", e.what());

		return 1;
	}

}
