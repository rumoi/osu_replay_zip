#pragma once

#include <string>
#include <vector>

#include "orz_shared.h"
#include "orz_tables.h"

namespace orz {

struct _seed_frame {
	bool present;
	u32 value;
};

struct _life_bar {
	bool present;
	std::string text;
};

struct orz_state {

	u32 options{}; // ORZ_OPTION

	// osu!standard: the setups of the player's earlier replays, as hints for the next encodes, the likeliest first (not
	// reset by reset_volatile)
	std::vector<dspace::_setup> hints;

	// the setup the last osu!standard encode's device model stands for (orz_setup.h), unknown parts 0
	dspace::_setup setup{};

	_orz_file file{};

	std::vector<u8> osr_header, after_life, tail;
	_life_bar life{};
	_seed_frame seed{};

	std::span<const u8> lzma{};

	float header_rate{}; // the play rate the .osr's mods give (double time 1.5, half time 0.75), 0 without an .osr
	float play_rate{ 1.f }; // header_rate, else 1: bare replay data does not say, and the device search tries double time too
	float clock_rate{}; // audio time against real time by the frames' 60 Hz ticks, 0 where the replay does not show it

	std::vector<u8> output;
	std::vector<_osr_frame> input;
	std::string text;

	// not reset by reset_volatile
	_table_memory tables{}; // can be reused by the next encode/decode

	struct {
		bool disable_device{};
	} debug;

	struct {
		size_t files{}, axis[5]{}, escapes{}, frames{}, segmented{};
	} stats;

	e_mode mode() const {

		return get_e_mode(file.encode_info);
	}

	// shortest round-trip strings
	bool exact_text() const {

		return (file.encode_info & ORZ_ENCODE_INFO::text_shortest) != 0;
	}
	
	void reset_volatile() {

		// options does not get reset

		file.version = 0;
		file.CRC = 0;
		file.encode_info = 0;
		file.seeded = 0;
		file.seed = 0;
		file.frame_count = 0;
		file.container.clear();
		file.container_custom.clear();

		osr_header.clear();
		after_life.clear();
		tail.clear();

		life.present = false;
		life.text.clear();

		seed = {};
		lzma = {};

		setup = {};
		header_rate = 0.f;
		play_rate = 1.f;
		clock_rate = 0.f;

		output.clear();
		input.clear();
		text.clear();

		stats = {};

	}

};

}
