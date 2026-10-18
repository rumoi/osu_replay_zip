#pragma once

#include <string>
#include <vector>

struct _seed_frame {
	bool present;
	u32 value;
};

struct _life_bar {
	bool present;
	std::string text;
};

struct _osr_bytes {
	bool present;
	std::vector<u8> bytes;
};

struct orz_state {

	u32 options{}; // ORZ_OPTION
	bool exact_text{}; // shortest round-trip strings
	bool catcher{}; // relax osu!catch: the cursor drives the catcher

	std::vector<u8> osr_header, after_life, tail;
	_life_bar life{};
	_seed_frame seed{};
	_osr_bytes replay_data{};

	std::vector<u8> output;
	const char* error{}; // why the encoder made no output
	size_t input_size{};
	std::vector<_osr_frame> input;
	std::string text;

	struct {
		bool disable_device{};
	} debug;

	struct {
		size_t files{}, axis[5]{}, escapes{}, frames{}, segmented{};
	} stats;

};
