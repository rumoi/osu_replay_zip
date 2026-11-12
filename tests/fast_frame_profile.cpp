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
	constexpr u32 fast{ ORZ_ENCODE_INFO::fast_frames }, verbatim{ ORZ_ENCODE_INFO::replay_verbatim };
	const std::vector<u8> legacy{ 0xe6, 0x80, 0x67, 0x45, 0x23, 0x01, 0x00, 0x00, 0x00 };
	require(envelope(0, 0) == legacy); // existing V0 header bits remain unchanged

	for (u32 mode{}; mode < 8; ++mode) {
		for (u32 text : { 0u, u32(trailing_comma), u32(text_shortest), u32(trailing_comma | text_shortest) }) {
			accepted(0, mode | text);
			accepted(1, mode | text | fast);
			accepted(1, mode | text | fast | has_osr_header);
		}
		accepted(0, mode | verbatim);
		rejected(0, mode | fast);
		rejected(1, mode);
		rejected(1, mode | verbatim | fast);
		rejected(2, mode | fast);
		require(set_e_mode(fast | text_shortest, e_mode(mode)) == (fast | text_shortest | mode));
	}
	for (u32 mode{ 8 }; mode < 16; ++mode) {
		rejected(0, mode);
		rejected(1, mode | fast);
	}
	for (u32 unknown : { 1u << 9, 1u << 31 }) {
		rejected(0, unknown);
		rejected(1, unknown | fast);
	}
	require(!is_newer_orz(envelope(1, fast)) && is_newer_orz(envelope(2, fast)));

	// Exercise final version selection and CRC coverage without selecting a frame-body model.
	for (u32 info : { 0u, fast }) {
		orz_state state;
		state.file.encode_info = info;
		finish_orz_file(0, ORZ_CRC_SEED, state);
		_orz_file file{};
		require(file.decode(state.output));
		require(file.version == (info == fast ? 1u : 0u));
		require(file.CRC == crc32c(ORZ_CRC_SEED, &info, sizeof(info)));
	}

	// Public entry resets preserve the requested option; an opaque input remains ordinary V0 verbatim.
	const std::vector<u8> opaque{ 1, 2, 3 };
	orz_state state;
	state.options = ENCODE_FAST_FRAME_DECODE;
	std::vector<u8> bytes;
	require(replay_data_to_orz(opaque, e_mode::osu, bytes, state));
	_orz_file file{};
	require(file.decode(bytes));
	require(file.version == 0 && file.encode_info == verbatim);
	require(state.options == ENCODE_FAST_FRAME_DECODE);
	state.reset_volatile();
	require(state.options == ENCODE_FAST_FRAME_DECODE && state.file.encode_info == 0);
	state.file.encode_info = fast | trailing_comma | text_shortest;
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

size_t check_replay(e_mode mode, const std::string& text, bool full, bool verbatim = false) {

	const auto replay{ compress_osr_string(text) };
	const auto input{ full ? full_osr(replay, mode) : replay };
	orz_state state;
	_table_memory tables;
	std::vector<u8> normal, fast, packed, restored, fresh_restored;
	size_t fast_streams{};

	for (const u32 options : { 0u, u32(ENCODE_FAST_FRAME_DECODE),
		u32(ENCODE_FAST_FRAME_DECODE | ENCODE_CHECK_ROUND_TRIP), 0u }) {

		state.options = options;
		require(full ? osr_to_orz(input, packed, state) : replay_data_to_orz(input, mode, packed, state));
		require(state.options == options);
		_decoded_orz reused, fresh;
		require(orz_decode_frames(packed, reused, tables) && orz_decode_frames(packed, fresh));
		require(same_frames(reused, fresh) && replay_text(reused.frames, reused.file) == text);
		const bool stored{ (reused.file.encode_info & replay_verbatim) != 0 };
		const bool requested{ (options & ENCODE_FAST_FRAME_DECODE) != 0 };
		require(!verbatim || stored);
		require(bool(reused.file.encode_info & fast_frames) == (requested && !stored));
		require(reused.file.version == (requested && !stored ? 1u : 0u));
		fast_streams += requested && !stored;
		require(orz_to_osr(packed, restored, tables) && orz_to_osr(packed, fresh_restored));
		require(restored == input && fresh_restored == input);

		if (!requested) {
			if (normal.empty()) normal = packed;
			else require(normal == packed); // fast calls must not leak state into normal coding
		} else {
			if (fast.empty()) fast = packed;
			else require(fast == packed); // roundtrip checking must not trigger an unnoticed fallback
		}

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
	size_t fast_streams{};

	// Autopilot (mode 7) is chosen by fitting recorded cursor paths, not by the .osr mod flag.
	for (u32 m{}; m < 7; ++m) {
		const auto mode{ e_mode(m) };
		std::string text;
		for (int i{}; i < 32; ++i) {
			const u32 keys{ mode == e_mode::mania ? 0u : u32(i % 4) };
			const std::string x{ mode == e_mode::mania ? std::to_string(i % 8) : i == 3 ? "-12.5" :
				game_mode(mode) == 1 ? std::to_string((int)modes::taiko_x(keys)) : std::to_string(256 + (i * 7) % 41) };
			const std::string y{ i == 3 ? "-4" : game_mode(mode) == 1 || game_mode(mode) == 2 ? "9999" : "192" };
			text += "16|" + x + "|" + y + "|" + std::to_string(keys) + ",";
		}
		text += "-12345|0|0|12345,";
		for (bool full : { false, true }) {
			const size_t coded{ check_replay(mode, text, full) };
			require(coded == 2); // each mode/entrypoint must exercise V1, without verbatim fallback
			fast_streams += coded;
		}
	}

	require(fast_streams != 0);
	for (bool full : { false, true }) {
		check_replay(e_mode::osu, "", full, true);
		check_replay(e_mode::osu, "-12345|0|0|12345,", full);
		check_replay(e_mode::osu, "16|256|192|0,", full);
		check_replay(e_mode::taiko, "0001|1.00|0|0,17|0|0|0,", full, true);
	}

	std::cout << "fast frame profile checks passed\n";
	return 0;
}
