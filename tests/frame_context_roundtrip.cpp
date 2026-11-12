// clang -O2 -ILib -c Lib/LzmaDec.c -o /tmp/LzmaDec.o
// clang++ -std=c++20 -O2 -I. -ILib tests/frame_context_roundtrip.cpp /tmp/LzmaDec.o -o /tmp/frame_context_roundtrip
// Exercise model changes with movement, unusual mode coordinates and verbatim replay text.

#include "orz_shared.h"
#include "orz_write.h"
#include "orz_read.h"

using namespace orz;

void require(bool ok) {

	if (!ok)
		throw std::runtime_error("frame context round trip failed");

}

void check(e_mode mode, const std::string& text, bool verbatim = false) {

	const auto input{ compress_osr_string(text) };
	std::vector<u8> packed, restored;
	orz_state state;
	_decoded_orz decoded;

	require(!input.empty());
	require(replay_data_to_orz(input, mode, packed, state));
	require(orz_decode_frames(packed, decoded));
	require(replay_text(decoded.frames, decoded.file) == text);
	require(!verbatim || (decoded.file.encode_info & ORZ_ENCODE_INFO::replay_verbatim));
	require(orz_to_osr(packed, restored));
	require(restored == input);

}

int main() {

	std::string movement;

	for (int i{}; i < 320; ++i)
		movement += std::to_string(i % 5 ? 16 : 33) + "|" + std::to_string(256 + (i * i * 7) % 101 - 50) +
			"|" + std::to_string(192 + (i * i * 11) % 137 - 68) + "|" + std::to_string(i % 7 ? 0 : 1) + ",";

	movement += "-12345|0|0|12345,";
	check(e_mode::osu, movement);
	check(e_mode::osu_relax, movement);
	check(e_mode::taiko, "0|256|192|0,17|12.5|-4|5,16|0|0|0,33|256|192|1,-12345|0|0|12345,");
	check(e_mode::fruits, "0|256|192|0,17|-1000000|1.25|1,16|1000000|-4|0,33|256|192|0,-12345|0|0|12345,");
	check(e_mode::mania, "0|0|192|0,17|1.5|192|1,16|-1000000|-4|0,33|5|192|0,-12345|0|0|12345,");

	for (const auto mode : { e_mode::taiko, e_mode::fruits, e_mode::mania }) {

		// The numeric values parse, but their spelling cannot be reproduced by either float printer.
		check(mode, "0001|1.00|0|0,17|0|0|0,-12345|0|0|12345,", true);
		check(mode, "1|1E+30|-1E+30|0,17|0|0|0,-12345|0|0|12345,");

	}

}
