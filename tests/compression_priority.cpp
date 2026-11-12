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

	constexpr u32 verbatim{ ORZ_ENCODE_INFO::replay_verbatim };
	const std::vector<u8> size_header{ 0xe6, 0x80, 0x67, 0x45, 0x23, 0x01, 0x00, 0x00, 0x00 };
	require(envelope(0, 0) == size_header);

	for (u32 version{}; version < 4; ++version)
		for (u32 priority{}; priority < 4; ++priority)
			for (u32 mode{}; mode < 16; ++mode)
				for (u32 flags : { 0u, u32(has_osr_header | trailing_comma | text_shortest), verbatim }) {

					const u32 info{ mode | flags | (priority << 8) };
					const bool valid{ version == 0 && mode < 8 && priority < 3 && (flags != verbatim || priority == 0) };
					if (valid) accepted(version, info);
					else rejected(version, info);

				}

	for (u32 unknown : { 1u << 10, 1u << 31 })
		for (u32 priority{}; priority < 3; ++priority)
			rejected(0, unknown | (priority << 8));

	require(!is_newer_orz(envelope(0, 0)) && is_newer_orz(envelope(1, 0)));

	for (u32 priority{}; priority < 3; ++priority) {

		const u32 info{ priority << 8 };
		orz_state state;
		state.file.encode_info = info;
		finish_orz_file(0, ORZ_CRC_SEED, state);
		_orz_file file{};
		require(file.decode(state.output));
		require(file.version == 0 && file.encode_info == info);
		require(file.CRC == crc32c(ORZ_CRC_SEED, &info, sizeof(info)));

	}

	const std::vector<u8> opaque{ 1, 2, 3 };
	orz_state state;
	std::vector<u8> bytes;
	_orz_file file{};
	for (u32 priority{}; priority < 3; ++priority) {

		const u32 options{ (priority << 3) | ENCODE_NO_DEVICE };
		state.options = options;
		require(replay_data_to_orz(opaque, e_mode::osu, bytes, state));
		require(file.decode(bytes));
		require(file.version == 0 && file.encode_info == verbatim);
		require(state.options == options);
		state.reset_volatile();
		require(state.options == options && state.file.encode_info == 0);

	}
	state.options = ENCODE_COMPRESSION_PRIORITY_MASK; // reserved value 3, even for verbatim input
	bytes = { 1 };
	require(!replay_data_to_orz(opaque, e_mode::osu, bytes, state) && bytes.empty());
	state.file.encode_info = compression_priority_mask | trailing_comma | text_shortest;
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
				const u32 index{ FALLBACK_PRIOR_VALUES[j] >> 24 };
				require(index < model->table.t.size());
				require(index < model->table.width || model == &standard.move);
			}
		}
		cursor = start[models.size()];
	}
	require(std::max(cursor, size_t{ 1 }) == std::size(FALLBACK_PRIOR_VALUES)); // empty generated arrays have a dummy entry

}

std::array<size_t, 3> check_replay(e_mode mode, const std::string& text, bool full, bool verbatim = false) {

	const auto replay{ compress_osr_string(text) };
	const auto input{ full ? full_osr(replay, mode) : replay };
	orz_state state;
	_table_memory tables;
	std::array<std::vector<u8>, 3> profiles;
	std::vector<u8> packed, restored, fresh_restored;
	std::array<size_t, 3> coded_streams{};

	for (const u32 options : { 0u, u32(ENCODE_COMPRESSION_PRIORITY_BALANCED),
		u32(ENCODE_COMPRESSION_PRIORITY_SPEED), u32(ENCODE_COMPRESSION_PRIORITY_BALANCED | ENCODE_CHECK_ROUND_TRIP),
		u32(ENCODE_COMPRESSION_PRIORITY_SPEED | ENCODE_CHECK_ROUND_TRIP), u32(ENCODE_CHECK_ROUND_TRIP), 0u }) {

		state.options = options;
		require(full ? osr_to_orz(input, packed, state) : replay_data_to_orz(input, mode, packed, state));
		require(state.options == options);
		_decoded_orz reused, fresh;
		require(orz_decode_frames(packed, reused, tables) && orz_decode_frames(packed, fresh));
		require(same_frames(reused, fresh) && replay_text(reused.frames, reused.file) == text);
		// Encoder-only flags cannot override a stored priority when passed to the decode API.
		require(orz_decode_frames(packed, fresh, tables, ENCODE_COMPRESSION_PRIORITY_SPEED | ENCODE_NO_DEVICE));
		require(same_frames(reused, fresh));
		const bool stored{ (reused.file.encode_info & replay_verbatim) != 0 };
		const u32 priority{ (options & ENCODE_COMPRESSION_PRIORITY_MASK) >> 3 };
		require(!verbatim || stored);
		require((reused.file.encode_info & compression_priority_mask) == (stored ? 0u : priority << 8));
		require(reused.file.version == 0);
		if (!stored)
			++coded_streams[priority];
		require(orz_to_osr(packed, restored, tables) && orz_to_osr(packed, fresh_restored));
		require(restored == input && fresh_restored == input);

		auto& expected{ profiles[priority] };
		if (expected.empty()) expected = packed;
		else require(expected == packed); // profile toggles and roundtrip checking preserve bytes

		_orz_file damaged{ reused.file };
		damaged.CRC ^= 1;
		std::vector<u8> bad;
		require(damaged.encode(bad));
		require(!orz_decode_frames(bad, fresh, tables));
		require(orz_decode_frames(bad, fresh, tables, DECODE_SKIP_CRC) && same_frames(reused, fresh));
	}

	// The public flag follows the established device-search bypass, including after profile/table reuse.
	for (u32 priority{}; priority < 3; ++priority) {

		state.options = (priority << 3) | ENCODE_NO_DEVICE;
		state.debug.disable_device = false;
		require(full ? osr_to_orz(input, packed, state) : replay_data_to_orz(input, mode, packed, state));
		_decoded_orz decoded;
		require(orz_decode_frames(packed, decoded, tables) && replay_text(decoded.frames, decoded.file) == text);
		require(orz_to_osr(packed, restored, tables) && restored == input);
		const auto no_device{ packed };
		state.options = priority << 3;
		state.debug.disable_device = true;
		require(full ? osr_to_orz(input, packed, state) : replay_data_to_orz(input, mode, packed, state));
		require(packed == no_device);

	}

	state.options = ENCODE_COMPRESSION_PRIORITY_MASK;
	require(!(full ? osr_to_orz(input, packed, state) : replay_data_to_orz(input, mode, packed, state)) && packed.empty());
	return coded_streams;
}

std::string fixture_text(e_mode mode) {
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
	return text;
}

int main() {

	check_wire();
	check_fallback_priors();

	// Autopilot (mode 7) is chosen by fitting recorded cursor paths, not by the .osr mod flag.
	for (u32 m{}; m < 7; ++m) {
		const auto mode{ e_mode(m) };
		const std::string text{ fixture_text(mode) };
		for (bool full : { false, true }) {
			const auto coded{ check_replay(mode, text, full) };
			require(coded[0] == 3 && coded[1] == 2 && coded[2] == 2); // exercise every priority, without verbatim fallback
		}
	}

	for (bool full : { false, true }) {
		check_replay(e_mode::osu, "", full, true);
		check_replay(e_mode::osu, "-12345|0|0|12345,", full);
		check_replay(e_mode::osu, "16|256|192|0,", full);
		check_replay(e_mode::taiko, "0001|1.00|0|0,17|0|0|0,", full, true);
	}

	std::cout << "compression priority checks passed\n";
	return 0;
}
