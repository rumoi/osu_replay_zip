// clang++ -std=c++20 -O2 -I. -ILib tests/fast_frame_profile.cpp /tmp/LzmaDec.o -o /tmp/fast_frame_profile
// The frame profile is selected by the file; encoder options do not override decoding.
#include "orz_shared.h"
#include "orz_write.h"
#include "orz_read.h"
#include <iostream>

using namespace orz;

void require(bool ok) {
	if (!ok)
		throw std::runtime_error("wire check failed");
}

std::vector<u8> envelope(u32 version, u32 info) {
	_orz_file file{};
	file.version = version;
	file.encode_info = info;
	file.CRC = 0x12345678;
	std::vector<u8> bytes;
	require(file.encode(bytes));
	return bytes;
}

void accepted(u32 version, u32 info) {
	_orz_file file{};
	require(file.decode(envelope(version, info)));
	require(file.version == version && file.encode_info == info);
}

void rejected(u32 version, u32 info) {
	const auto bytes{ envelope(version, info) };
	_orz_file file{};
	require(!file.decode(bytes)); // rejection happens before any frame-model dispatch
	_decoded_orz frames;
	require(!orz_decode_frames(bytes, frames, DECODE_SKIP_CRC));
}

void check_wire() {
	constexpr u32 fast{ ORZ_ENCODE_INFO::fast_frames }, fastest{ ORZ_ENCODE_INFO::fastest_frames };
	constexpr u32 verbatim{ ORZ_ENCODE_INFO::replay_verbatim };
	const std::vector<u8> legacy{ 0xe6, 0x80, 0x67, 0x45, 0x23, 0x01, 0x00, 0x00, 0x00 };
	require(envelope(0, 0) == legacy); // existing V0 header bits remain unchanged

	for (u32 mode{}; mode < 8; ++mode) {
		for (u32 text : { 0u, u32(trailing_comma), u32(text_shortest), u32(trailing_comma | text_shortest) }) {
			accepted(0, mode | text);
			accepted(1, mode | text | fast);
			accepted(1, mode | text | fast | has_osr_header);
			accepted(1, mode | text | fast | fastest);
			accepted(1, mode | text | fast | fastest | has_osr_header);
		}
		accepted(0, mode | verbatim);
		rejected(0, mode | fast);
		rejected(0, mode | fastest);
		rejected(0, mode | fast | fastest);
		rejected(1, mode);
		rejected(1, mode | fastest);
		rejected(1, mode | verbatim | fast);
		rejected(1, mode | verbatim | fast | fastest);
		rejected(2, mode | fast);
		rejected(2, mode | fast | fastest);
		require(set_e_mode(fast | fastest | text_shortest, e_mode(mode)) == (fast | fastest | text_shortest | mode));
	}
	for (u32 mode{ 8 }; mode < 16; ++mode) {
		rejected(0, mode);
		rejected(1, mode | fast);
		rejected(1, mode | fast | fastest);
	}
	for (u32 unknown : { 1u << 10, 1u << 31 }) {
		rejected(0, unknown);
		rejected(1, unknown | fast);
		rejected(1, unknown | fast | fastest);
	}
	require(!is_newer_orz(envelope(1, fast)) && is_newer_orz(envelope(2, fast)));

	// Exercise final version selection and CRC coverage without selecting a frame-body model.
	for (u32 info : { 0u, fast, fast | fastest }) {
		orz_state state;
		state.file.encode_info = info;
		finish_orz_file(0, ORZ_CRC_SEED, state);
		_orz_file file{};
		require(file.decode(state.output));
		require(file.version == (info ? 1u : 0u));
		require(file.CRC == crc32c(ORZ_CRC_SEED, &info, sizeof(info)));
	}

	// Public entry resets preserve the requested option; an opaque input remains ordinary V0 verbatim.
	const std::vector<u8> opaque{ 1, 2, 3 };
	orz_state state;
	std::vector<u8> bytes;
	_orz_file file{};
	for (u32 options : { u32(ENCODE_FAST_FRAME_DECODE), u32(ENCODE_FASTEST_FRAME_DECODE),
		u32(ENCODE_FAST_FRAME_DECODE | ENCODE_FASTEST_FRAME_DECODE) }) {
		state.options = options;
		require(replay_data_to_orz(opaque, e_mode::osu, bytes, state));
		require(file.decode(bytes));
		require(file.version == 0 && file.encode_info == verbatim);
		require(state.options == options);
		state.reset_volatile();
		require(state.options == options && state.file.encode_info == 0);
	}
	state.file.encode_info = fast | fastest | trailing_comma | text_shortest;
	state.lzma = opaque;
	write_verbatim(state);
	require(file.decode(state.output));
	require(file.version == 0 && file.encode_info == verbatim);
}

std::vector<u8> full_osr(std::span<const u8> replay, e_mode mode) {

	std::vector<u8> out{ (u8)game_mode(mode) };
	add_to_u8(out, 20200101u);

	// Three valid strings keep the synthetic header above read_osr_header's minimum size.
	for (int i{}; i < 3; ++i) {
		out.push_back(0x0b);
		add_uleb128(out, 32);
		out.insert(out.end(), 32, (u8)('a' + i));
	}

	out.resize(out.size() + 19); // hit counts, score, combo, perfect
	add_to_u8(out, is_relax(mode) ? OSR_MOD_RELAX : 0u);
	out.push_back(0); // no life bar
	out.resize(out.size() + 8); // timestamp
	add_to_u8(out, (u32)replay.size());
	out.insert(out.end(), replay.begin(), replay.end());
	out.insert(out.end(), { 1, 2, 3, 4, 5, 6, 7, 8 }); // online score ID
	return out;
}

bool same_frames(const _decoded_orz& a, const _decoded_orz& b) {
	return a.frames.size() == b.frames.size() &&
		(a.frames.empty() || std::memcmp(a.frames.data(), b.frames.data(), a.frames.size() * sizeof(_osr_frame)) == 0);
}

void check_fallback_priors() {

	require(std::size(FALLBACK_PRIOR_START) == 11);
	require(std::size(FALLBACK_PRIOR_KEYS) == std::size(FALLBACK_PRIOR_VALUES));
	const u32 first{ (u32)cm::hash(cm::FALLBACK_INPUT, 0) };
	const u32 second{ (u32)cm::hash(cm::MAX_INPUTS + cm::FALLBACK_INPUT, 0) };
	size_t cursor{};

	for (u32 set{}; set < std::size(FALLBACK_PRIOR_START); ++set) {
		frames::_models standard;
		modes::_models other;
		std::array<cm::model*, FRAMES_PRIOR_MODELS> models;
		if (set < frames::set_taiko) {
			const bool relax{ set >= frames::set_relax_rational && set <= frames::set_relax_other };
			standard.init(0, relax ? e_mode::osu_relax : e_mode::osu, false, true);
			models = standard.all();
		} else {
			other.init(0, modes::kind(set - frames::set_taiko), false, true);
			models = other.all();
		}
		const auto& start{ FALLBACK_PRIOR_START[set] };
		require(std::size(start) == models.size() + 1 && start[0] == cursor);
		for (size_t m{}; m < models.size(); ++m) {
			require(start[m] <= start[m + 1] && start[m + 1] <= std::size(FALLBACK_PRIOR_VALUES));
			for (u32 j{ start[m] }; j < start[m + 1]; ++j) {
				const auto* model{ models[m] };
				require(model && !model->table.t.empty());
				require((FALLBACK_PRIOR_VALUES[j] >> 24) < model->table.width);
				const u32 key{ FALLBACK_PRIOR_KEYS[j] };
				require(key == first || (key == second && model == &standard.move));
			}
		}
		cursor = start[models.size()];
	}
	require(std::max(cursor, size_t{ 1 }) == std::size(FALLBACK_PRIOR_VALUES)); // empty generated arrays have a dummy entry

}

std::array<size_t, 2> check_replay(e_mode mode, const std::string& text, bool full, bool verbatim = false) {

	const auto replay{ compress_osr_string(text) };
	const auto input{ full ? full_osr(replay, mode) : replay };
	orz_state state;
	_table_memory tables;
	std::array<std::vector<u8>, 3> profiles;
	std::vector<u8> packed, restored, fresh_restored;
	std::array<size_t, 2> fast_streams{};

	for (const u32 options : { 0u, u32(ENCODE_FAST_FRAME_DECODE),
		u32(ENCODE_FASTEST_FRAME_DECODE), u32(ENCODE_FAST_FRAME_DECODE | ENCODE_FASTEST_FRAME_DECODE),
		u32(ENCODE_FASTEST_FRAME_DECODE | ENCODE_CHECK_ROUND_TRIP),
		u32(ENCODE_FAST_FRAME_DECODE | ENCODE_CHECK_ROUND_TRIP), 0u }) {

		state.options = options;
		require(full ? osr_to_orz(input, packed, state) : replay_data_to_orz(input, mode, packed, state));
		require(state.options == options);
		_decoded_orz reused, fresh;
		require(orz_decode_frames(packed, reused, tables) && orz_decode_frames(packed, fresh));
		require(same_frames(reused, fresh) && replay_text(reused.frames, reused.file) == text);
		const bool stored{ (reused.file.encode_info & replay_verbatim) != 0 };
		const bool fastest{ (options & ENCODE_FASTEST_FRAME_DECODE) != 0 };
		const bool requested{ fastest || (options & ENCODE_FAST_FRAME_DECODE) != 0 };
		require(!verbatim || stored);
		require(bool(reused.file.encode_info & fast_frames) == (requested && !stored));
		require(bool(reused.file.encode_info & fastest_frames) == (fastest && !stored));
		require(reused.file.version == (requested && !stored ? 1u : 0u));
		if (requested && !stored)
			++fast_streams[fastest];
		require(orz_to_osr(packed, restored, tables) && orz_to_osr(packed, fresh_restored));
		require(restored == input && fresh_restored == input);

		auto& expected{ profiles[fastest ? 2 : requested ? 1 : 0] };
		if (expected.empty()) expected = packed;
		else require(expected == packed); // profile toggles, combined options and roundtrip checking preserve bytes

		_orz_file damaged{ reused.file };
		damaged.CRC ^= 1;
		std::vector<u8> bad;
		require(damaged.encode(bad));
		require(!orz_decode_frames(bad, fresh, tables));
		require(orz_decode_frames(bad, fresh, tables, DECODE_SKIP_CRC) && same_frames(reused, fresh));
	}

	return fast_streams;
}

int main() {

	check_wire();
	check_fallback_priors();

	// Autopilot (mode 7) is chosen by fitting recorded cursor paths, not by the .osr mod flag.
	for (u32 m{}; m < 7; ++m) {
		const auto mode{ e_mode(m) };
		std::string text;
		u32 random{ 1234567 };
		for (int i{}; i < 128; ++i) {
			random = random * 1664525u + 1013904223u;
			const u32 keys{ mode == e_mode::mania || mode == e_mode::fruits ? 0u :
				game_mode(mode) == 1 ? (random >> 24) & 3u : u32(i % 4) };
			const int walk{ (32 + i) % 128 }; // catch walks 8 px per 16 ms, turning at the playfield edges
			const std::string x{ mode == e_mode::mania ? std::to_string((random >> 24) & 7u) : i == 3 ? "-12.5" :
				game_mode(mode) == 1 ? std::to_string((int)modes::taiko_x(keys)) :
				mode == e_mode::fruits ? std::to_string((walk <= 64 ? walk : 128 - walk) * 8) : std::to_string(256 + i) };
			const std::string y{ i == 3 ? "-4" : game_mode(mode) == 1 || game_mode(mode) == 2 ? "9999" : "192" };
			text += "16|" + x + "|" + y + "|" + std::to_string(keys) + ",";
		}
		text += "-12345|0|0|12345,";
		for (bool full : { false, true }) {
			const auto coded{ check_replay(mode, text, full) };
			require(coded[0] == 2 && coded[1] == 3); // exercise both V1 profiles, without verbatim fallback
		}
	}

	for (bool full : { false, true }) {
		check_replay(e_mode::osu, "", full, true);
		check_replay(e_mode::osu, "-12345|0|0|12345,", full);
		check_replay(e_mode::osu, "16|256|192|0,", full);
		check_replay(e_mode::taiko, "0001|1.00|0|0,17|0|0|0,", full, true);
	}

	std::cout << "fast frame profile checks passed\n";
	return 0;
}
