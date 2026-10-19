#pragma once

#include "orz_shared.h"
#include "orz_life.h"
#include "orz_parts.h"
#include "orz_text.h"
#include "orz_encoder_std.h"
#include "orz_modes.h"

namespace orz {

// the data of tag, empty when the file has none
static std::span<const u8> container_data(const _orz_file& file, orz_tag tag) {

	for (const auto& c : file.container)
		if (c.tag == tag)
			return c.data;

	return {};
}

struct _decoded_orz {
	_orz_file file;
	std::vector<_osr_frame> frames;
};

static bool decode_frames(std::span<const u8> raw_bytes, const u32 options, _decoded_orz& out) {

	_orz_file file{};

	if (!file.decode(raw_bytes) || file.frame_count > ORZ_MAX_FRAMES)
		return false;

	const u32 info{ file.encode_info };

	const e_mode mode{ get_e_mode(info) };

	const bool verbatim{ (info & ORZ_ENCODE_INFO::replay_verbatim) != 0 }; // there is no frames stream

	std::vector<_osr_frame> R(file.frame_count);

	if (!verbatim) {

		const auto frames_data{ container_data(file, orz_tag::frames) };

		if (!(uses_modes_coder(mode) ? modes::decode(info, frames_data, R) : standard::decode(info, frames_data, R)))
			return false;

	}

	// readd the closing seed frame
	if (file.seeded)
		R.push_back({ -12345, 0.f, 0.f, file.seed });

	if (verbatim) {

		const auto data{ container_data(file, orz_tag::osr_verbatim) };

		std::string text;

		// a score saved without a replay (no data) or data that never decompressed
		R = decompress_osr_string(data.data(), data.size(), text) ?
			orz_text::parse_frames(text.data(), text.size()) :
			std::vector<_osr_frame>{};

	}

	if (!(options & DECODE_SKIP_CRC)) {

		u32 crc{ crc32c(ORZ_CRC_SEED, R.data(), R.size() * sizeof(_osr_frame)) };

		for (const orz_tag tag : CRC_PART_TAGS)
			if (const auto part{ container_data(file, tag) }; part.size())
				crc = crc32c_part(crc, part.data(), part.size());

		crc = crc32c(crc, &file.encode_info, sizeof(file.encode_info));

		if (crc != file.CRC)
			return false;

	}

	out = { std::move(file), std::move(R) };

	return true;
}

// an ORZ file held in memory into frames; false for anything that is not a valid file
bool orz_decode_frames(std::span<const u8> raw_bytes, _decoded_orz& out, const u32 options = 0) {

	no_FMA_check();

	try {
		return decode_frames(raw_bytes, options, out);
	} catch (const std::exception&) {
		return false;
	}

}

std::string replay_text(const std::vector<_osr_frame>& R, const _orz_file& file) {

	const u32 info{ file.encode_info };

	if (info & ORZ_ENCODE_INFO::replay_verbatim) {

		const auto data{ container_data(file, orz_tag::osr_verbatim) };

		std::string text;

		decompress_osr_string(data.data(), data.size(), text);

		return text;
	}

	std::string text{ orz_text::frames_text(R, (info & ORZ_ENCODE_INFO::text_shortest) != 0) };

	if (info & ORZ_ENCODE_INFO::trailing_comma)
		text += ',';

	return text;
}

static bool decode_to_osr(std::span<const u8> raw_bytes, std::vector<u8>& final_file_output, const u32 options) {

	_decoded_orz decoded;

	if (!orz_decode_frames(raw_bytes, decoded, options))
		return false;

	const auto& [file, R] { decoded };

	const u32 info{ file.encode_info };

	const auto parts{ osr_parts::decode(container_data(file, orz_tag::osr_meta_data)) };

	const auto verbatim{ container_data(file, orz_tag::osr_verbatim) };

	const std::vector<u8> compressed_string{ (info & ORZ_ENCODE_INFO::replay_verbatim) ?
		std::vector<u8>(verbatim.begin(), verbatim.end()) :
		compress_osr_string(replay_text(R, file))
	};

	final_file_output.clear();

	if (info & ORZ_ENCODE_INFO::has_osr_header) {

		// header chunk, life graph, chunk up to the replay data length

		add_to_u8(final_file_output, parts.header);

		if (parts.life.size()) {

			const std::string text{ life::decode(parts.life, R) };

			final_file_output.push_back(0x0b);

			add_uleb128(final_file_output, (u32)text.size());

			final_file_output.insert(final_file_output.end(), text.begin(), text.end());

		} else
			final_file_output.push_back(0x00);

		add_to_u8(final_file_output, parts.after_life);

		add_to_u8(final_file_output, (u32)compressed_string.size());

	}

	add_to_u8(final_file_output, compressed_string);

	// bytes after the replay data (online score id, extra data), verbatim
	add_to_u8(final_file_output, parts.tail);

	return true;
}

// an .orz into the .osr (or bare replay data) it came from
bool orz_to_osr(std::span<const u8> orz, std::vector<u8>& out, const u32 options = 0) {

	no_FMA_check();

	try {
		return decode_to_osr(orz, out, options);
	} catch (const std::exception&) {
		return false;
	}

}

bool orz_round_trip_ok(std::span<const u8> orz, const std::string& want) {

	_decoded_orz d;

	return orz_decode_frames(orz, d) && replay_text(d.frames, d.file) == want;
}

}
