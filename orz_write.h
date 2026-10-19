#pragma once

#include <algorithm>
#include <cmath>

#include "orz_shared.h"
#include "orz_state.h"
#include "orz_life.h"
#include "orz_parts.h"
#include "orz_text.h"

namespace orz {

// a stream as a container of the file (an empty one is left out)
inline void add_container(_orz_file& file, orz_tag tag, std::span<const u8> data) {

	if (data.size())
		file.container.push_back({ std::vector<u8>(data.begin(), data.end()), tag });

}

// the .osr parts in state as containers of its file: the bytes around the replay data with the life bar, and verbatim replay data
inline void add_osr_parts(orz_state& st) {

	if (st.osr_header.size() || st.after_life.size() || st.tail.size()) {

		const auto parts{ osr_parts::encode({ st.osr_header, st.after_life, st.tail,
			st.life.present ? life::encode(st.life.text, st.input) : std::vector<u8>{} }) };

		add_container(st.file, orz_tag::osr_meta_data, parts);

	}

	if (st.file.encode_info & ORZ_ENCODE_INFO::replay_verbatim)
		add_container(st.file, orz_tag::osr_verbatim, st.lzma);

}

// the CRC of the frames as the decoder returns them, the closing seed frame included
inline u32 frames_crc(std::span<const _osr_frame> frames, const _seed_frame& seed) {

	u32 crc{ crc32c(ORZ_CRC_SEED, frames.data(), frames.size() * sizeof(_osr_frame)) };

	if (seed.present)
		crc = crc32c_frame(crc, { -12345, 0.f, 0.f, seed.value });

	return crc;
}

inline void write_verbatim(orz_state& state);

// state.file with its .osr parts and header (the CRC continuing from crc over the .osr parts and the encode info) into
// state.output
inline void finish_orz_file(u32 frame_count, u32 crc, orz_state& state) {

	// degenerate cases could exist, if the .orz frame stream is larger than the input LZMA stream, fall back to verbatim
	if (!(state.file.encode_info & ORZ_ENCODE_INFO::replay_verbatim)) {

		for (const auto& c : state.file.container)
			if (c.tag == orz_tag::frames && c.data.size() >= state.lzma.size())
				return write_verbatim(state);

	}

	add_osr_parts(state);

	_orz_file& file{ state.file };

	file.version = ORZ_VERSION;
	file.seeded = state.seed.present;
	file.seed = state.seed.value;
	file.frame_count = frame_count;

	// the CRC continues over the stored .osr parts, in CRC_PART_TAGS order
	for (const orz_tag tag : CRC_PART_TAGS)
		for (const auto& c : file.container)
			if (c.tag == tag && c.data.size())
				crc = crc32c_part(crc, c.data.data(), c.data.size());

	file.CRC = crc32c(crc, &file.encode_info, sizeof(file.encode_info));

	file.encode(state.output);

}

// state.file as the LZMA replay data as it is: no frames, no seed, no text flags
inline void write_verbatim(orz_state& state) {

	state.seed = {};

	state.file.container.clear();
	state.file.encode_info &= ~u32(ORZ_ENCODE_INFO::trailing_comma | ORZ_ENCODE_INFO::text_shortest);
	state.file.encode_info |= ORZ_ENCODE_INFO::replay_verbatim;

	finish_orz_file(0, frames_crc(state.input, {}), state);

}

namespace standard { void encode(const _osr& r, orz_state& state); }
namespace modes { void encode(const _osr& r, orz_state& state); }

// the e_mode picks the coder: osu!catch relax maps the catcher to the cursor's x, so it goes through osu!standard's
void encode_replay(_osr& r, orz_state& state) {

	if (uses_modes_coder(state.mode()))
		return modes::encode(r, state);

	standard::encode(r, state);
}

bool orz_round_trip_ok(std::span<const u8> orz, const std::string& want, _table_memory& tables);

// an .osr's header: the e_mode its frames are coded as, and the parts the decoder needs to rebuild the file
struct _osr_header {

	e_mode mode;

	std::span<const u8> before_life; // the game mode up to the mods, stored as they are
	_life_bar life;
	std::span<const u8> after_life;  // the time stamp
	std::span<const u8> replay_data; // the LZMA replay data
	std::span<const u8> tail;        // what follows the replay data (online score id, extra data)

};

// false for anything that is not an .osr of one of the four game modes
inline bool read_osr_header(std::span<const u8> osr, _osr_header& h) {

	if (osr.size() < 128 || osr[0] > 3)
		return false;

	size_t pos{};

	const auto skip = [&](size_t n) {

		if (n > osr.size() - pos)
			return false;

		pos += n;

		return true;
	};

	const auto skip_string = [&]() {

		if (pos >= osr.size())
			return false;

		if (osr[pos++] != 0x0b)
			return true;

		u32 len{};

		for (int k{}; ; ++k) {

			if (pos >= osr.size() || k == 5)
				return false;

			const u8 c{ osr[pos++] };

			len |= u32(c & 0x7f) << (7 * k);

			if (!(c & 0x80))
				break;

		}

		return skip(len);
	};

	const bool ok {
		skip(1) && skip(4) && // gamemode, game version
		skip_string() && skip_string() && skip_string() // beatmap hash, player name, replay hash
	};

	if (!ok)
		return false;

	#pragma pack(push, 1)

	struct _osr_header_values {

		i16 hit_300, hit_100, hit_50, geki, katsu, misses;

		i32 score;
		u16 combo;
		u8 perfect;

		u32 mods;

	};

	#pragma pack(pop)

	const auto* header_values{ (const _osr_header_values*)(osr.data() + pos) };

	if (!skip(sizeof(_osr_header_values)))
		return false;

	// osu!mania has no relax
	const u32 game_mode{ osr[0] };
	const bool relax{ (header_values->mods & OSR_MOD_RELAX) != 0 && game_mode != 3 };

	h.mode = (e_mode)(relax ? (u32)e_mode::osu_relax + game_mode : game_mode);

	const size_t life_start{ pos };

	if (!skip_string())
		return false;

	const size_t life_end{ pos };

	h.life = {};

	if (life_end - life_start >= 2 && osr[life_start] == 0x0b) {

		size_t q{ life_start + 1 };

		while (q < life_end && (osr[q] & 0x80))
			++q;

		if (q < life_end)
			++q;

		h.life = { true, std::string((const char*)osr.data() + q, (const char*)osr.data() + life_end) };

	}

	if (!h.life.present && life_end > life_start && osr[life_start] != 0)
		h.life = { true, std::string() };

	if (!skip(8)) // time stamp
		return false;

	h.before_life = osr.first(life_start);
	h.after_life = osr.subspan(life_end, pos - life_end);

	if (pos + 4 > osr.size())
		return false;

	u32 compressed{};

	memcpy(&compressed, osr.data() + pos, 4);

	pos += 4;

	h.replay_data = osr.subspan(pos);
	h.tail = {};

	if (compressed <= h.replay_data.size()) {

		h.tail = h.replay_data.subspan(compressed);
		h.replay_data = h.replay_data.first(compressed);

	}

	return true;
}

// LZMA replay data into state.output, with the e_mode of state.file (and the .osr parts in state, when it has them)
static void encode_replay_data(std::span<const u8> lzma, orz_state& state) {

	// the span is the caller's, it does not outlive this encode
	state.lzma = lzma;

	ON_SCOPE_EXIT(state.lzma = {}; );

	if (!decompress_osr_string(lzma.data(), lzma.size(), state.text)) {

		write_verbatim(state);

		return;
	}

	_osr replay_stream{ orz_text::parse_frames(state.text.data(), state.text.size()) };

	state.input = replay_stream;

	if (replay_stream.size() > ORZ_MAX_FRAMES)
		replay_stream.clear(); // format can not store this many frames

	// only trust the frames if the text writer gives back this exact text, else the replay data is stored as is
	bool verbatim{};

	{

		std::string_view body{ state.text };

		if (body.size() && body.back() == ',') {

			state.file.encode_info |= ORZ_ENCODE_INFO::trailing_comma;
			body.remove_suffix(1);

		}

		if (replay_stream.empty() || orz_text::frames_text(replay_stream, false) != body) {

			if (orz_text::frames_text(replay_stream, true) == body)
				state.file.encode_info |= ORZ_ENCODE_INFO::text_shortest;
			else
				verbatim = true;

		}

	}

	{

		const float x_limit{ state.mode() == e_mode::mania ? 16777216.f : 131072.f };

		for (const auto& f : replay_stream)
			if (!(std::abs(f.x) <= x_limit) || !(std::abs(f.y) <= 131072.f)) // catches NaN too
				verbatim = true;

	}

	if (verbatim) {

		write_verbatim(state);

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

	encode_replay(replay_stream, state);

	const bool coded{ !(state.file.encode_info & ORZ_ENCODE_INFO::replay_verbatim) };

	if (coded && (state.options & ENCODE_CHECK_ROUND_TRIP) && !orz_round_trip_ok(state.output, state.text, state.tables))
		write_verbatim(state);

}

// raw replay data (the LZMA chunk of an .osr) of a replay of the given e_mode into an .orz; false (out empty) for input the
// encoder can not handle
bool replay_data_to_orz(std::span<const u8> lzma, e_mode mode, std::vector<u8>& out, orz_state& state) {

	no_FMA_check();

	try {

		state.reset_volatile();

		state.file.encode_info = set_e_mode(0, mode);

		encode_replay_data(lzma, state);

	} catch (const std::exception&) {
		state.output.clear();
	}

	out = std::move(state.output);

	return !out.empty();
}

// an .osr into an .orz; false (out empty) for anything the encoder can not handle
bool osr_to_orz(std::span<const u8> osr, std::vector<u8>& out, orz_state& state) {

	no_FMA_check();

	try {

		state.reset_volatile();

		if (_osr_header h{}; read_osr_header(osr, h)) {

			state.osr_header.assign(h.before_life.begin(), h.before_life.end());
			state.after_life.assign(h.after_life.begin(), h.after_life.end());
			state.tail.assign(h.tail.begin(), h.tail.end());
			state.life = std::move(h.life);

			state.file.encode_info = set_e_mode(ORZ_ENCODE_INFO::has_osr_header, h.mode);

			encode_replay_data(h.replay_data, state);

		}

	} catch (const std::exception&) {
		state.output.clear();
	}

	out = std::move(state.output);

	return !out.empty();
}

}
