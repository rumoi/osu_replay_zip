#pragma once

#include <algorithm>
#include <cmath>

void add_to_u8(std::vector<u8>& output, u32 input) {

	const auto i{ output.size() };

	output.resize(i + 4);

	memcpy(output.data() + i, &input, 4);
}

void add_to_u8(std::vector<u8>& output, const void* src, size_t size) {

	const auto i{ output.size() };

	output.resize(i + size);

	memcpy(output.data() + i, src, size);
}

template<typename T>
void add_to_u8(std::vector<u8>& output, const std::vector<T>& input) {

	constexpr auto SIZE{ sizeof(T) };

	const auto i{ output.size() };

	output.resize(i + input.size() * SIZE);

	memcpy(output.data() + i, input.data(), input.size() * SIZE);
}

std::pair<u32, u32>& unique_add(u32 v, std::vector<std::pair<u32, u32>>& b) {

	for (auto& t : b) {

		if (t.first == v) {

			++t.second;

			return t;
		}

	}

	return b.emplace_back(v, 1);
}

const auto pair_sort = [](const auto& x, const auto& y) {

	return x.second > y.second;
};

struct _orz_construct {

	constexpr static size_t MAX_ELEMENTS{ 256 };

	orz_state* state;

	u32 flags{}, frame_count{}, data_count{};
	u32 frames_crc{};

	orz_tag tag_table[MAX_ELEMENTS];
	u32 size_table[MAX_ELEMENTS];

	std::vector<u8> file_data;

	void add_stream(orz_tag tag, const void* src, size_t size) {

		if (data_count == 255 || size == 0) [[unlikely]]
			return;

		ON_SCOPE_EXIT( ++data_count; );

		tag_table[data_count] = tag;
		size_table[data_count] = (u32)size;

		add_to_u8(file_data, src, size);

	}

	void append(const _orz_construct& o) {

		for (size_t i{}; i < o.data_count && data_count < 255; ++i) {

			tag_table[data_count] = o.tag_table[i];
			size_table[data_count] = o.size_table[i];

			++data_count;

		}

		add_to_u8(file_data, o.file_data);

	}

	void write(const char* output_file) {

		add_osr_parts();
		finish(output_file);

	}

	void add_osr_parts() {

		const auto& st{ *state };

		if (st.osr_header.size() || st.after_life.size() || st.tail.size()) {

			const auto parts{ osr_parts::encode({ st.osr_header, st.after_life, st.tail,
				st.life.present ? life::encode(st.life.text, st.input) : std::vector<u8>{} }) };

			add_stream(orz_tag::osr_parts, parts.data(), parts.size());

		}

		if (st.replay_data.present)
			add_stream(orz_tag::osr_replay_data, st.replay_data.bytes.data(), st.replay_data.bytes.size());

	}

	static u32 crc_of(std::span<const _osr_frame> frames, const _seed_frame& seed) {

		u32 crc{ crc32c(ORZ_CRC_SEED, frames.data(), frames.size() * sizeof(_osr_frame)) };

		if (seed.present)
			crc = crc32c_frame(crc, { -12345, 0.f, 0.f, seed.value });

		return crc;
	}

	void finish(const char* output_file) {

		// a stream's bytes, empty when absent
		const auto stream = [&](orz_tag tag) {

			size_t offset{};

			for (size_t i{}; i < data_count; offset += size_table[i++])
				if (tag_table[i] == tag)
					return std::span<const u8>{ file_data.data() + offset, size_table[i] };

			return std::span<const u8>{};
		};

		// the CRC continues over the stored .osr parts, in CRC_PART_TAGS order
		u32 crc{ frames_crc };

		for (const orz_tag tag : CRC_PART_TAGS)
			if (const auto part{ stream(tag) }; part.size())
				crc = crc32c_part(crc, part.data(), part.size());

		const u32 stored_flags{ pack_flags(flags) };

		crc = crc32c(crc, &stored_flags, sizeof(stored_flags));

		bit_stream h{};

		write_bucket(ORZ_VERSION, h, 4, 8);
		write_bucket(stored_flags, h, 7);

		for (int k{}; k < 32; ++k)
			h.push_back((crc >> k) & 1);

		h.push_back(state->seed.present);

		if (state->seed.present)
			write_length_bucket<3, ORZ_SEED_LENGTH>(state->seed.value, h);

		write_bucket(frame_count, h, 16);

		// the streams go in a fixed order, so the flags say which are there: with an .osr its parts (life bar included),
		// then the replay (verbatim or frames), which runs to the end of the file
		const auto parts{ stream(orz_tag::osr_parts) };
		const auto replay{ stream((flags & ORZ_FLAG::replay_verbatim) ? orz_tag::osr_replay_data : orz_tag::frames) };

		// parts in the fields form take 44 to 72 bytes, and most with a life bar stay under 127
		if (flags & ORZ_FLAG::has_osr_header)
			write_bucket((u32)parts.size(), h, 7);

		std::vector<u8> final_file{};

		final_file.reserve(1 + h.data.size() + file_data.size());

		final_file.push_back(ORZ_MAGIC);

		add_to_u8(final_file, h.data);

		for (const auto s : { parts, replay })
			add_to_u8(final_file, s.data(), s.size());

		if (strlen(output_file))
			write_file(output_file, final_file);

		state->output = std::move(final_file);

	}

};

namespace standard { void encode(const char* output_file, const _osr& r, const u32 flags, orz_state& state); }
namespace modes { void encode(const char* output_file, const _osr& r, const u32 flags, orz_state& state); }

void encode_replay(const char* output_file, _osr& r, const u32 flags, orz_state& state, bool relax) {

	// osu!mania has no relax
	const u32 f{ relax && !(flags & ORZ_FLAG::gamemode_mania) ? flags | ORZ_FLAG::relax : flags };

	// CTB replays with relax map the catcher to the x of the cursor, encode the replay through the standard encoder to catch this
	if ((flags & (ORZ_FLAG::gamemode_mania | ORZ_FLAG::gamemode_taiko)) || ((flags & ORZ_FLAG::gamemode_fruits) && !relax))
		return modes::encode(output_file, r, f, state);

	standard::encode(output_file, r, f, state);
}

bool orz_round_trip_ok(const std::vector<u8>& orz, const std::string& want);

static void encode_osr(const std::vector<u8>& raw_file, const char* out_file, const u32 header_info, orz_state& state) {

	{

		orz_state fresh{};

		fresh.options = state.options;
		fresh.debug = state.debug;

		state = std::move(fresh);

	}

	if (is_orz(raw_file)) {

		state.error = "already an .orz file";

		return;
	}

	// header info stands in for missing .osr headers, never next to them
	const bool header{ starts_with_osr_header(raw_file) }, given{ (header_info & HEADER_INFO_GIVEN) != 0 };
	const u32 info_mode{ (header_info >> 1) & 3 };
	bool relax{ ((header_info >> 3) & 1) != 0 };

	if (header && given) {

		state.error = "the input has .osr headers, so header info must not be given";

		return;
	}

	if (!header && !given) {

		state.error = "the input has no .osr headers, and no header info (game mode, relax) was given";

		return;
	}

	if (!header && info_mode == 3 && relax) {

		state.error = "osu!mania has no relax";

		return;
	}

	if (raw_file.size() < 128)
		return;

	u32 flags{ header ? (u32)ORZ_FLAG::has_osr_header : 1u << info_mode };

	const size_t file_size{ raw_file.size() };

	size_t pos{};

	const auto skip = [&](size_t n) {

		if (n > file_size - pos)
			return false;

		pos += n;

		return true;
	};

	const auto skip_string = [&]() {

		if (pos >= file_size)
			return false;

		if (raw_file[pos++] != 0x0b)
			return true;

		u32 len{};

		for (int k{}; ; ++k) {

			if (pos >= file_size || k == 5)
				return false;

			const u8 c{ raw_file[pos++] };

			len |= u32(c & 0x7f) << (7 * k);

			if (!(c & 0x80))
				break;

		}

		return skip(len);
	};

	size_t lzma_offset{}, lzma_size{ file_size };

	auto& header_bytes{ state.osr_header };

	if (header) {

		flags |= 1u << raw_file[0];

		const bool ok {
			skip(1) && skip(4) && // gamemode, game version
			skip_string() && skip_string() && skip_string() // beatmap hash, player name, replay hash
		};

		if (!ok)
			return;

		#pragma pack(push, 1)

		struct _osr_header_values {

			i16 hit_300, hit_100, hit_50, geki, katsu, misses;

			i32 score;
			u16 combo;
			u8 perfect;

			u32 mods;

		};

		#pragma pack(pop)

		const auto* header_values{ (const _osr_header_values*)(raw_file.data() + pos) };

		if (!skip(sizeof(_osr_header_values)))
			return;

		relax = (header_values->mods & OSR_MOD_RELAX) != 0;

		const size_t life_start{ pos };

		if (!skip_string())
			return;

		const size_t life_end{ pos };

		if (life_end - life_start >= 2 && raw_file[life_start] == 0x0b) {

			size_t q{ life_start + 1 };

			while (q < life_end && (raw_file[q] & 0x80))
				++q;

			if (q < life_end)
				++q;

			state.life = { true, std::string((const char*)raw_file.data() + q, (const char*)raw_file.data() + life_end) };

		}

		if (!skip(8)) // time stamp
			return;

		header_bytes.assign(raw_file.begin(), raw_file.begin() + life_start);

		state.after_life.assign(raw_file.begin() + life_end, raw_file.begin() + pos);

		if (!state.life.present && life_end > life_start && raw_file[life_start] != 0)
			state.life = { true, std::string() };

		if (pos + 4 > file_size)
			return;

		u32 compressed{};

		memcpy(&compressed, raw_file.data() + pos, 4);

		pos += 4;

		lzma_offset = pos;
		lzma_size = file_size - pos;

		if (compressed <= lzma_size) {

			state.tail.assign(raw_file.begin() + pos + compressed, raw_file.end());
			lzma_size = compressed;

		}

	}

	const u8* const lzma_start{ raw_file.data() + lzma_offset };

	const auto write_verbatim = [&]() {

		state.seed = {};
		state.exact_text = false;
		state.replay_data = { true, std::vector<u8>(lzma_start, lzma_start + lzma_size) };

		_orz_construct result{ &state };

		result.flags = flags | ORZ_FLAG::replay_verbatim;
		result.frames_crc = _orz_construct::crc_of(state.input, {});
		result.write(out_file);

	};

	state.input_size = lzma_size;

	if (!decompress_osr_string(lzma_start, lzma_size, state.text)) {

		write_verbatim();

		return;
	}

	_osr replay_stream{ orz_text::parse_frames(state.text.data(), state.text.size()) };

	state.input = replay_stream;

	if (replay_stream.size() > ORZ_MAX_FRAMES)
		replay_stream.clear(); // format can not store this many frames

	// only trust the frames if the text writer gives back this exact text, else the replay data is stored as is
	u32 text_flags{};

	state.exact_text = false;

	{

		std::string_view body{ state.text };

		if (body.size() && body.back() == ',') {

			text_flags |= ORZ_FLAG::trailing_comma;
			body.remove_suffix(1);

		}

		if (replay_stream.empty() || orz_text::frames_text(replay_stream, state.exact_text) != body) {

			state.exact_text = true;

			if (orz_text::frames_text(replay_stream, state.exact_text) == body)
				text_flags |= ORZ_FLAG::text_shortest;
			else {

				state.exact_text = false;
				text_flags |= ORZ_FLAG::replay_verbatim;

			}

		}

	}

	{

		const float x_limit{ (flags & ORZ_FLAG::gamemode_mania) ? 16777216.f : 131072.f };

		for (const auto& f : replay_stream)
			if (!(std::abs(f.x) <= x_limit) || !(std::abs(f.y) <= 131072.f)) // catches NaN too
				text_flags |= ORZ_FLAG::replay_verbatim;

	}

	if (text_flags & ORZ_FLAG::replay_verbatim) {

		write_verbatim();

		return;
	}

	state.seed = {};

	if (replay_stream.size()) {

		const auto& b{ replay_stream.back() };

		if (b.delta == -12345 && b.x == 0.f && b.y == 0.f) {

			state.seed = { true, b.keys };
			replay_stream.pop_back();

		}

	}

	encode_replay(out_file, replay_stream, flags | text_flags, state, relax);

	if ((state.options & ENCODE_CHECK_ROUND_TRIP) && !orz_round_trip_ok(state.output, state.text))
		write_verbatim();

}

// an .osr, or bare replay data with header_info saying what it is (header_info()), in memory: the .orz goes to
// state.output; when there is none, state.error says why
void osr_to_orz(const std::vector<u8>& input, const u32 header_info, orz_state& state) {

	try {
		encode_osr(input, "", header_info, state);
	} catch (const std::exception&) {
		state.output.clear(); // nothing is written for input the encoder can not handle
	}

	if (state.output.empty() && !state.error)
		state.error = "not a valid replay";

}

void osr_to_orz(const char* in_file, const char* out_file, const u32 header_info, orz_state& state) {

	try {
		encode_osr(read_file(in_file), out_file, header_info, state);
	} catch (const std::exception&) {
		state.output.clear();
		state.error = nullptr;
	}

	if (state.output.empty() && !state.error)
		state.error = "not a valid replay";

}
