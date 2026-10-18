#pragma once

#include "orz_shared.h"

struct _stream_map {

	struct _elem {
		u8 tag;
		std::vector<u8> data;
	};

	std::vector<_elem> data;

	const std::vector<u8>& operator[](orz_tag tag) const {

		for (const auto& v : data) {

			if (v.tag != u8(tag))
				continue;

			return v.data;
		}

		static const std::vector<u8> empty{};

		return empty;
	}

};

namespace standard { bool decode(const _orz_header* header, const _stream_map& stream_data, std::vector<_osr_frame>& R); }
namespace modes { bool decode(const _orz_header* header, const _stream_map& stream_data, std::vector<_osr_frame>& R); }

struct _decoded_orz {
	_stream_map streams;
	std::vector<_osr_frame> frames;
	u32 flags{};
};

static bool decode_frames(const std::vector<u8>& raw_bytes, const u32 options, _decoded_orz& out) {

	const auto container{ read_container(raw_bytes) };

	if (!container.valid)
		return false;

	const _orz_header* const header{ &container.header };

	_stream_map stream_data{};

	stream_data.data.resize(container.streams.size());

	for (size_t i{}; i < container.streams.size(); ++i) {

		const auto& s{ container.streams[i] };
		auto& o{ stream_data.data[i] };
		const u8* const data{ raw_bytes.data() + s.offset };

		o.tag = s.tag;
		o.data.assign(data, data + s.size);

	}

	std::vector<_osr_frame> R(header->frame_count);

	const bool other_mode{ kind_other_frames(info_kind(header->flags)) };
	const bool verbatim{ (header->flags & ORZ_ENCODE_INFO::replay_verbatim) != 0 }; // there is no frames stream

	if (!verbatim && !(other_mode ? modes::decode(header, stream_data, R) : standard::decode(header, stream_data, R)))
		return false;

	// readd the closing seed frame
	if (header->seeded)
		R.push_back({ -12345, 0.f, 0.f, header->seed });

	if (verbatim) {

		const auto& data{ stream_data[orz_tag::osr_replay_data] };

		std::string text;

		// a score saved without a replay (no data) or data that never decompressed
		R = decompress_osr_string(data.data(), data.size(), text) ?
			orz_text::parse_frames(text.data(), text.size()) :
			std::vector<_osr_frame>{};

	}

	if (!(options & DECODE_SKIP_CRC)) {

		u32 crc{ crc32c(ORZ_CRC_SEED, R.data(), R.size() * sizeof(_osr_frame)) };

		for (const orz_tag tag : CRC_PART_TAGS)
			if (const auto& part{ stream_data[tag] }; part.size())
				crc = crc32c_part(crc, part.data(), part.size());

		crc = crc32c(crc, &header->flags, sizeof(header->flags));

		if (crc != header->crc)
			return false;

	}

	out = { std::move(stream_data), std::move(R), header->flags };

	return true;
}

// an ORZ file held in memory into frames; false for anything that is not a valid file
bool orz_decode_frames(const std::vector<u8>& raw_bytes, _decoded_orz& out, const u32 options = 0) {

	try {
		return decode_frames(raw_bytes, options, out);
	} catch (const std::exception&) {
		return false;
	}

}

std::string replay_text(const std::vector<_osr_frame>& R, u32 flags, const _stream_map& stream_data) {

	if (flags & ORZ_ENCODE_INFO::replay_verbatim) {

		const auto& data{ stream_data[orz_tag::osr_replay_data] };

		std::string text;

		decompress_osr_string(data.data(), data.size(), text);

		return text;
	}

	std::string text{ orz_text::frames_text(R, (flags & ORZ_ENCODE_INFO::text_shortest) != 0) };

	if (flags & ORZ_ENCODE_INFO::trailing_comma)
		text += ',';

	return text;
}

static bool decode_to_osr(const std::vector<u8>& raw_bytes, std::vector<u8>& final_file_output, const u32 options) {

	_decoded_orz decoded;

	if (!orz_decode_frames(raw_bytes, decoded, options))
		return false;

	const auto& [stream_data, R, flags] { decoded };

	const auto parts{ osr_parts::decode(stream_data[orz_tag::osr_parts]) };

	const std::vector<u8> compressed_string{ (flags & ORZ_ENCODE_INFO::replay_verbatim) ?
		stream_data[orz_tag::osr_replay_data] :
		compress_osr_string(replay_text(R, flags, stream_data))
	};

	final_file_output.clear();

	if (flags & ORZ_ENCODE_INFO::has_osr_header) {

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

		add_to_u8(final_file_output, compressed_string.size());

	}

	add_to_u8(final_file_output, compressed_string);

	// bytes after the replay data (online score id, extra data), verbatim
	add_to_u8(final_file_output, parts.tail);

	return true;
}

// an .orz in memory: the .osr (or bare replay data) goes to out
bool orz_to_osr(const std::vector<u8>& orz, std::vector<u8>& out, const u32 options = 0) {

	try {
		return decode_to_osr(orz, out, options);
	} catch (const std::exception&) {
		return false;
	}

}

bool orz_to_osr(const char* input_file, const char* output_file, const u32 options = 0) {

	try {
		std::vector<u8> out;
		return decode_to_osr(read_file(input_file), out, options) && write_file(output_file, out);
	} catch (const std::exception&) {
		return false;
	}

}

bool orz_round_trip_ok(const std::vector<u8>& orz, const std::string& want) {

	_decoded_orz d;

	return orz_decode_frames(orz, d) && replay_text(d.frames, d.flags, d.streams) == want;
}

#include "orz_time.h"
#include "orz_encoder_std.h"
#include "orz_modes.h"
