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

	_orz_file file{};

	std::vector<u8> osr_header, after_life, tail;
	_life_bar life{};
	_seed_frame seed{};

	std::span<const u8> lzma{};

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

		output.clear();
		input.clear();
		text.clear();

		stats = {};

	}

};

}
