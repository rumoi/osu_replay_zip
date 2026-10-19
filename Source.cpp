#define _CRT_SECURE_NO_WARNINGS

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "orz_shared.h"
#include "orz_write.h"
#include "orz_read.h"

template<typename T>
bool write_file(const char* file_name, const T& data) {

	std::ofstream file(file_name, std::ios::binary);

	if (!file.is_open())
		return false;

	file.write((const char*)data.data(), data.size());

	return file.good();
}

std::vector<orz::u8> read_file(const char* file_name) {

	std::ifstream file(file_name, std::ios::binary | std::ios::ate | std::ios::in);

	if (!file.is_open()) [[unlikely]]
		return {};

	const size_t file_size{ (size_t)file.tellg() };

	std::vector<orz::u8> ret(file_size);

	if (file_size) [[likely]] {

		file.seekg(0, std::ios::beg);
		file.read((char*)ret.data(), file_size);

	}

	return ret;
}

static bool is_orz(const std::vector<orz::u8>& data) {
	return data.size() >= 2 && data[0] == orz::ORZ_MAGIC;
}

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

	std::vector<orz::u8> result;

	if (decode) {

		if (!orz::orz_to_osr(data, result)) {

			if (orz::is_newer_orz(data))
				fprintf(stderr, "error: %s was made by a newer version of orz\n", in_s.c_str());
			else if (orz::_decoded_orz d; orz::orz_decode_frames(data, d, orz::DECODE_SKIP_CRC))
				fprintf(stderr, "error: %s fails its CRC check (damaged, or written by an incompatible orz build)\n", in_s.c_str());
			else
				fprintf(stderr, "error: %s is not a valid .orz file\n", in_s.c_str());

			return 1;
		}

	} else {

		orz::orz_state state{};

		if (!orz::osr_to_orz(data, result, state)) {

			fprintf(stderr, "error: %s is not a valid .osr file\n", in_s.c_str());

			return 1;
		}

	}

	if (!write_file(out_s.c_str(), result)) {

		fprintf(stderr, "error: can not write %s\n", out_s.c_str());

		return 1;
	}

	if (!decode)
		printf("%s -> %s (reduction %ld%%)\n", in_s.c_str(), out_s.c_str(), std::lround(100.0 - 100.0 * result.size() / data.size()));

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
