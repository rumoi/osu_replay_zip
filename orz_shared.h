#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <emmintrin.h>
#include <nmmintrin.h>
#include <fstream>
#include <new>
#include <span>
#include <string>
#include <vector>

#if defined(_M_FP_FAST) || defined(_M_FP_CONTRACT) || defined(__FAST_MATH__)
#error "breaks without strict IEEE float maths (/fp:precise)"
#endif
#if defined(_M_IX86) && _M_IX86_FP < 2
#error "needs SSE2 floating point (/arch:SSE2)"
#endif
static_assert(FLT_EVAL_METHOD == 0, "float must not use extended precision (x87)");

#if defined(_MSC_VER)
#define FORCEINLINE __forceinline
#define TARGET_SSE42
#else
#define FORCEINLINE inline __attribute__((always_inline))
#define TARGET_SSE42 __attribute__((target("sse4.2"))) // the CRC instructions, without building everything for SSE 4.2
#endif

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;

// MSVC generates terrible code for std::bit_cast<float>(u32) - have to use this
FORCEINLINE float float_from_bits(u32 bits) {
	return _mm_cvtss_f32(_mm_castsi128_ps(_mm_cvtsi32_si128((int)bits)));
}

// same float bits (tells -0 from 0, NaNs apart)
FORCEINLINE bool same_bits(float a, float b) {
	return std::bit_cast<u32>(a) == std::bit_cast<u32>(b);
}

constexpr u32 ORZ_VERSION{ 0 };

// limits for untrusted input, real replays are far below all of them and anything beyond is treated as corrupted
constexpr size_t ORZ_MAX_FRAMES{ 1u << 22 }; // (~19 hours at 60 fps)
constexpr size_t ORZ_MAX_STREAM{ 1u << 28 };

enum ORZ_FLAG {

	gamemode_std = 1 << 0,
	gamemode_taiko = 1 << 1,
	gamemode_fruits = 1 << 2,
	gamemode_mania = 1 << 3,

	has_osr_header = 1 << 4,

	trailing_comma = 1 << 5,

	replay_verbatim = 1 << 6,

	text_shortest = 1 << 7, // floats are 'shortest round-trip' (newer .NET, osu!lazer)

	// frames of the other modes (orz_modes.h), else osu!standard's (orz_frames.h)
	mania_columns = 1 << 8, // osu!mania: held columns in x, scroll speed in y
	taiko_keys = 1 << 9, // osu!taiko: positions from the keys
	fruits_catcher = 1 << 10, // osu!catch: catcher positions against a movement prediction

	relax = 1 << 11, // relax replay: osu!standard frame times use the replay's own delta table, relax priors

	autopilot_path = 1 << 12, // osu!standard frames coded with the autopilot path model (orz_autopilot.h)

};

// what a replay is, as one number in the stored flags: the game mode, relax or autopilot (osu! only, frames coded with the
// autopilot path model), and so the form of its frames. osu!catch relax is coded as osu!standard frames (the cursor
// drives the catcher), as are osu!mania replays that do not fit the column form (stored as osu!)
constexpr u32 FLAG_KINDS[]{
	ORZ_FLAG::gamemode_std,
	ORZ_FLAG::gamemode_std | ORZ_FLAG::relax,
	ORZ_FLAG::gamemode_std | ORZ_FLAG::autopilot_path,
	ORZ_FLAG::gamemode_taiko | ORZ_FLAG::taiko_keys,
	ORZ_FLAG::gamemode_taiko | ORZ_FLAG::taiko_keys | ORZ_FLAG::relax,
	ORZ_FLAG::gamemode_fruits | ORZ_FLAG::fruits_catcher,
	ORZ_FLAG::gamemode_fruits | ORZ_FLAG::relax,
	ORZ_FLAG::gamemode_mania | ORZ_FLAG::mania_columns,
};

// the flags as a file stores them: the kind (FLAG_KINDS), then has_osr_header, trailing_comma, replay_verbatim and
// text_shortest a bit each. The game modes are checked in the order the other modes' coder takes them.
inline u32 pack_flags(u32 f) {

	const u32 rx{ (f & ORZ_FLAG::relax) != 0 };
	const u32 kind{ (f & ORZ_FLAG::gamemode_mania) ? 7 : (f & ORZ_FLAG::gamemode_taiko) ? 3 + rx : (f & ORZ_FLAG::gamemode_fruits) ? 5 + rx :
		rx ? 1 : (f & ORZ_FLAG::autopilot_path) ? 2 : 0 };

	return kind | ((f >> 4) & 15) << 3;
}

inline u32 unpack_flags(u32 s) {
	return FLAG_KINDS[s & 7] | ((s >> 3) & 15) << 4;
}

enum ORZ_OPTION : u32 {

	ENCODE_CHECK_ROUND_TRIP = 1 << 0,

	DECODE_SKIP_CRC = 1 << 2,

	ENCODE_AUTOPILOT = 1 << 4,

};

constexpr u32 OSR_MOD_RELAX{ 1 << 7 };

// what headerless replay data is, from the caller, since only an .osr's header says: bit 0 set when given, bits 1-2
// the game mode, bit 3 relax. 0 gives none, for input with .osr headers
constexpr u32 HEADER_INFO_GIVEN{ 1 };

constexpr u32 header_info(u32 mode, bool relax) {
	return HEADER_INFO_GIVEN | (mode & 3) << 1 | u32(relax) << 3;
}

// an .osr starts with its game mode and the client version (yyyymmdd); bare replay data starts with LZMA properties
inline bool starts_with_osr_header(std::span<const u8> data) {

	if (data.size() < 5 || data[0] > 3)
		return false;

	i32 version;

	memcpy(&version, data.data() + 1, 4);

	return version >= 20070000 && version <= 30070000;
}

enum class orz_tag : u8 {

	// delta table and the mode's model, then the context mixed frames: orz_frames.h (osu!standard), orz_modes.h (other modes)
	frames = 0,

	osr_parts, // the .osr bytes around the replay data: the header, the life bar, what follows it, what follows the replay data (orz_parts.h)
	osr_replay_data,

};

// replay data is decoded with the LZMA SDK's decoder (Lib), and encoded the way osu! encodes it (osu_lzma.h)
#include "LzmaDec.h"
#include "osu_lzma.h"

struct _lzma_block_cache {

	static constexpr size_t SLOTS{ 8 }, MAX_BLOCK{ 1 << 22 }, HEADER{ 16 };

	u8* block[SLOTS]{};
	size_t size[SLOTS]{};

	~_lzma_block_cache() {

		for (u8* b : block)
			delete[] b;

	}
};

static _lzma_block_cache& lzma_cache() {

	thread_local _lzma_block_cache cache;

	return cache;
}

static void* _lzmaAlloc(ISzAllocPtr, size_t size) {

	auto& c{ lzma_cache() };

	for (size_t k{}; k < c.SLOTS; ++k) {

		if (c.block[k] && c.size[k] >= size && c.size[k] / 2 <= size) {

			u8* b{ c.block[k] };

			c.block[k] = nullptr;

			return b + c.HEADER;
		}

	}

	u8* b{ new (std::nothrow) u8[size + c.HEADER] };

	if (!b)
		return nullptr;

	memcpy(b, &size, sizeof size);

	return b + c.HEADER;
}

static void _lzmaFree(ISzAllocPtr, void* addr) {

	if (!addr)
		return;

	auto& c{ lzma_cache() };

	u8* b{ reinterpret_cast<u8*>(addr) - c.HEADER };

	size_t size;

	memcpy(&size, b, sizeof size);

	if (size <= c.MAX_BLOCK) {

		for (size_t k{}; k < c.SLOTS; ++k) {

			if (!c.block[k]) {

				c.block[k] = b;
				c.size[k] = size;

				return;
			}

		}

	}

	delete[] b;

}

static ISzAlloc lzma_allocFuncs = {
	_lzmaAlloc, _lzmaFree
};

#ifndef ON_SCOPE_EXIT

template <typename F> struct on_scope_exit {
private: F func;
public:
	on_scope_exit(const on_scope_exit&) = delete;
	on_scope_exit& operator=(const on_scope_exit&) = delete;

	constexpr on_scope_exit(F&& f) : func(std::forward<F>(f)) {}
	constexpr ~on_scope_exit() { func(); }
};

#define PCAT0(x, y) PCAT1(x, y)
#define PCAT1(x, y) PCAT2(!, x ## y)
#define PCAT2(x, r) r
#define ON_SCOPE_EXIT(...) on_scope_exit PCAT0(__scope, __LINE__) {[&]{__VA_ARGS__}}

#endif

template<typename T>
bool write_file(const char* file_name, const T& data) {

	std::ofstream file(file_name, std::ios::binary);

	if (!file.is_open())
		return false;

	file.write((const char*)data.data(), data.size());

	return true;
}

std::vector<u8> read_file(const char* file_name) {

	std::ifstream file(file_name, std::ios::binary | std::ios::ate | std::ios::in);

	if (!file.is_open()) [[unlikely]]
		return {};

	const size_t file_size{ (size_t)file.tellg() };

	std::vector<u8> ret(file_size);

	if (file_size) [[likely]] {

		file.seekg(0, std::ios::beg);
		file.read((char*)ret.data(), file_size);

	}

	return ret;
}

struct bit_stream {

	std::vector<u8> data;
	u8 in_count{};

	void clear() {

		in_count = 0;
		data.clear();

	}

	size_t max_size() const {
		return data.size() * 8;
	}

	size_t size() const {

		size_t r{ in_count };

		if (data.size() > 0)
			r += (data.size() - 1) * 8;

		return r;
	}

	void push_back(const bool v) {

		if (in_count > 7 || data.size() == 0) {

			in_count = 0;
			data.push_back({});

		}

		data.back() |= u8(v) << (in_count++);

	}

	// bits past the end read as 0, so a short or damaged stream can never read outside data
	bool operator[](size_t i) const {

		const size_t bucket{ i >> 3 };

		return bucket < data.size() && ((data[bucket] >> (i & 7)) & 1);
	}

	void append(const bit_stream& v) {

		for (size_t i{}, size{ v.size() }; i < size; ++i)
			push_back(v[i]);

	}

};

struct _bit_view {

	std::span<const u8> data;

	_bit_view(std::span<const u8> bytes) : data{ bytes } {}
	_bit_view(const bit_stream& bits) : data{ bits.data } {}

	size_t max_size() const {
		return data.size() * 8;
	}

	bool operator[](size_t i) const {

		const size_t bucket{ i >> 3 };

		return bucket < data.size() && ((data[bucket] >> (i & 7)) & 1);
	}

};

// raw float, 32 bits
inline void write_float(float v, bit_stream& o) {

	const u32 b{ std::bit_cast<u32>(v) };

	for (int i{}; i < 32; ++i)
		o.push_back((b >> i) & 1);

}

inline float read_float(const _bit_view in, size_t& i) {

	u32 b{};

	for (int k{}; k < 32; ++k)
		b |= u32(in[i++]) << k;

	return float_from_bits(b);
}

template<size_t BIT_WRITE>
void small_write(u32 value, bit_stream& output) {

	if constexpr (BIT_WRITE == 1)
		return output.push_back(value);

	constexpr auto MAX_VALUE{ (1 << BIT_WRITE) - 1 };

	while (value >= MAX_VALUE) {

		for (size_t i{}; i < BIT_WRITE; ++i)
			output.push_back(1);

		const auto in{ 31 - std::countl_zero(value) };

		// only the bits under the most significant bit are stored
		for (size_t i{ BIT_WRITE - 1 }; i < in; ++i)
			output.push_back(1);

		output.push_back(0);

		for (size_t i{}; i < in; ++i)
			output.push_back((value >> i) & 1);

		return;
	}

	for (size_t i{}; i < BIT_WRITE; ++i)
		output.push_back((value >> i) & 1);

}

template<size_t BIT_WRITE>
u32 small_read(const _bit_view input, size_t& i) {

	if (i >= input.max_size())
		return 0;

	if constexpr (BIT_WRITE == 1)
		return input[i++];

	constexpr auto MAX_VALUE{ (1 << BIT_WRITE) - 1 };

	u32 value{}, c{};

	for (size_t size{ input.max_size() }; c < BIT_WRITE && (i + c) < size; ++c)
		value |= u32(input[i + c]) << c;

	i += c;
	c = 0;

	if (value != MAX_VALUE)
		return value;

	int bit_count{ BIT_WRITE - 1 };

	for (size_t size{ input.max_size() }; (i + c) < size; ++c)
		if (input[i + c] == 0)
			break;

	bit_count += c;

	if (bit_count > 31)
		bit_count = 31;

	i += c + 1;
	c = 0;
	value = 1 << bit_count;

	for (size_t size{ input.max_size() }; c < bit_count && (i + c) < size; ++c)
		value |= u32(input[i + c]) << c;

	i += bit_count;

	return value;
}

template<u32 OVERFLOW_BITS = 5, typename ...T >
void write_bucket(u32 value, bit_stream& output, const T... bits) {

	bool done{};

	const auto write = [&](const size_t b) {

		if (b == 0 || done)
			return;

		const u32 MAX_VALUE{ (1u << b) - 1 };

		if (value < MAX_VALUE) {

			for (size_t i{}; i < b; ++i)
				output.push_back((value >> i) & 1);

			done = true;

			return;
		}

		for (size_t i{}; i < b; ++i)
			output.push_back(1);

		value -= MAX_VALUE;

	};

	(write(bits), ...);

	if (done)
		return;

	value += 1;

	const auto in{ (size_t)31 - std::countl_zero(value) };

	for (size_t i{}; i < OVERFLOW_BITS; ++i)
		output.push_back((in >> i) & 1);

	for (size_t i{}; i < in; ++i)
		output.push_back((value >> i) & 1);

}

template<u32 OVERFLOW_BITS = 5, typename ...T >
u32 read_bucket(const _bit_view input, size_t& i, const T... bits) {

	u32 result{};

	const auto max_size{ input.max_size() };

	bool done{};

	const auto read = [&](const size_t b) {

		if (b == 0 || done)
			return;

		u32 v{};

		ON_SCOPE_EXIT(result += v; );

		for (size_t z{}; z < b && i < max_size; ++z)
			v |= u32(input[i++] != 0) << z;

		if (v != (1u << b) - 1)
			done = true;

	};

	(read(bits), ...);

	if (done)
		return result;

	u32 bit_count{};

	for (size_t z{}; z < OVERFLOW_BITS && i < max_size; ++z)
		bit_count |= u32(input[i++] != 0) << z;

	u32 v{ (u32)1 << bit_count };

	for (size_t z{}; z < bit_count && i < max_size; ++z)
		v |= u32(input[i++] != 0) << z;

	return result + v - 1;
}

template<u32 LENGTH_BITS, u32 FROM>
void write_length_bucket(u32 value, bit_stream& output) {

	constexpr u32 ESCAPE{ (1u << LENGTH_BITS) - 1 };
	constexpr u32 REST_BITS{ (u32)std::bit_width(32u - ESCAPE) };

	static_assert(FROM + ESCAPE <= 33);

	const u32 length{ (u32)std::bit_width(value) };
	const u32 code{ std::min(length - FROM, ESCAPE) };

	for (u32 k{}; k < LENGTH_BITS; ++k)
		output.push_back((code >> k) & 1);

	if (code == ESCAPE) {

		const u32 rest{ length < FROM ? length : length - ESCAPE };

		for (u32 k{}; k < REST_BITS; ++k)
			output.push_back((rest >> k) & 1);

	}

	for (u32 k{}; k + 1 < length; ++k)
		output.push_back((value >> k) & 1);

}

template<u32 LENGTH_BITS, u32 FROM>
u32 read_length_bucket(const _bit_view input, size_t& i) {

	constexpr u32 ESCAPE{ (1u << LENGTH_BITS) - 1 };
	constexpr u32 REST_BITS{ (u32)std::bit_width(32u - ESCAPE) };

	const auto max_size{ input.max_size() };

	u32 code{};

	for (u32 k{}; k < LENGTH_BITS && i < max_size; ++k)
		code |= u32(input[i++] != 0) << k;

	u32 length{ code + FROM };

	if (code == ESCAPE) {

		u32 rest{};

		for (u32 k{}; k < REST_BITS && i < max_size; ++k)
			rest |= u32(input[i++] != 0) << k;

		length = std::min(rest < FROM ? rest : rest + ESCAPE, 32u);

	}

	u32 value{ length ? 1u << (length - 1) : 0u };

	for (u32 k{}; k + 1 < length && i < max_size; ++k)
		value |= u32(input[i++] != 0) << k;

	return value;
}

constexpr u8 ORZ_MAGIC{ 'r' };

inline bool is_orz(std::span<const u8> data) {
	return data.size() >= 2 && data[0] == ORZ_MAGIC;
}

// tuned to sessions under around 4.7 hours
constexpr u32 ORZ_SEED_LENGTH{ 18 };

struct _orz_header {
	u32 version, flags, frame_count;
	u32 crc; // of the decoded replay
	bool seeded;
	u32 seed; // of the closing -12345 frame
};

struct _orz_container {

	struct _stream {
		u8 tag;
		u32 size;
		size_t offset;
	};

	bool valid{};
	bool newer{};
	_orz_header header{};
	std::vector<_stream> streams;
	size_t header_bytes{};
};

inline _orz_container read_container(const std::vector<u8>& file) {

	_orz_container c{};

	if (file.size() < 2 || file[0] != ORZ_MAGIC)
		return c;

	// every header field at its largest still comes to under 300 bits, so 4 KB always holds the header
	const _bit_view bs{ std::span{ file }.subspan(1, std::min<size_t>(file.size() - 1, 4096)) };

	size_t i{};
	auto& h{ c.header };

	h.version = read_bucket(bs, i, 4, 8);

	// we are trying to read a orz file from the future. abort.
	if (h.version > ORZ_VERSION) {

		c.newer = true;

		return c;
	}

	h.flags = unpack_flags(read_bucket(bs, i, 7));

	for (int k{}; k < 32; ++k)
		h.crc |= u32(small_read<1>(bs, i)) << k;

	h.seeded = small_read<1>(bs, i) != 0;

	if (h.seeded)
		h.seed = read_length_bucket<3, ORZ_SEED_LENGTH>(bs, i);

	h.frame_count = read_bucket(bs, i, 16);

	if (h.frame_count > ORZ_MAX_FRAMES)
		return c;

	// the streams in their fixed order (_orz_construct::finish)
	if (h.flags & ORZ_FLAG::has_osr_header)
		c.streams.push_back({ (u8)orz_tag::osr_parts, read_bucket(bs, i, 7) });

	c.streams.push_back({ (u8)((h.flags & ORZ_FLAG::replay_verbatim) ? orz_tag::osr_replay_data : orz_tag::frames) });

	c.header_bytes = 1 + (i + 7) / 8;

	if (c.header_bytes > file.size())
		return c;

	size_t offset{ c.header_bytes };

	for (auto& s : c.streams) {

		// the replay runs to the end of the file
		if (&s == &c.streams.back())
			s.size = (u32)(file.size() - offset);

		if (s.size > file.size() - offset)
			return c;

		s.offset = offset;
		offset += s.size;

	}

	c.valid = true;

	return c;
}

struct _osr_frame {
	int delta;
	float x, y;
	u32 keys;
};

using _osr = std::vector<_osr_frame>;

constexpr float POS_DEFAULT_X{ 256.f }, POS_DEFAULT_Y{ -500.f };

static_assert(sizeof(_osr_frame) == 16, "replay_crc hashes frames as 16 bytes");

TARGET_SSE42 inline u32 crc32c(u32 crc, const void* data, size_t size) {

	const u8* p{ (const u8*)data };

#if defined(_M_X64) || defined(__x86_64__)

	u64 c{ crc };

	for (; size >= 8; size -= 8, p += 8) {

		u64 v;

		memcpy(&v, p, 8);

		c = _mm_crc32_u64(c, v);

	}

	u32 c32{ (u32)c };

#else

	u32 c32{ crc };

	for (; size >= 4; size -= 4, p += 4) {

		u32 v;

		memcpy(&v, p, 4);

		c32 = _mm_crc32_u32(c32, v);

	}

#endif

	for (; size; --size)
		c32 = _mm_crc32_u8(c32, *p++);

	return c32;
}

// where the replay CRC starts: not 0, so an all-zero header (a cut file reads as zeros) does not check out
constexpr u32 ORZ_CRC_SEED{ 0xFFFFFFFF };

FORCEINLINE u32 crc32c_frame(u32 crc, const _osr_frame& f) {
	return crc32c(crc, &f, sizeof f);
}

constexpr orz_tag CRC_PART_TAGS[]{
	orz_tag::osr_parts,
	orz_tag::osr_replay_data
};

FORCEINLINE u32 crc32c_part(u32 crc, const u8* data, size_t size) {

	const u32 n{ (u32)size };

	return crc32c(crc32c(crc, &n, 4), data, size);
}

inline bool decompress_osr_string(const u8* data, size_t size, std::string& out) {

	out.clear();

	if (size <= 5 + 8)
		return false;

	u32 text_size;

	memcpy(&text_size, data + 5, 4);

	if (text_size > ORZ_MAX_STREAM)
		return false;

	out.resize(text_size);

	SizeT out_size{ text_size }, in_size{ size - (5 + 8) };
	ELzmaStatus status{};

	LzmaDecode((u8*)out.data(), &out_size, data + 5 + 8, &in_size, data, 5, LZMA_FINISH_END, &status, &lzma_allocFuncs);

	if (out_size != text_size) {

		out.clear();

		return false;
	}

	return true;
}

// the replay text as osu! stores it, so replay data the game wrote comes back byte for byte (osu_lzma.h)
inline std::vector<u8> compress_osr_string(std::string_view string) {
	return osu_lzma::compress({ (const u8*)string.data(), string.size() });
}

#include "orz_state.h"

#include "device_space.h"
#include "orz_life.h"
#include "orz_parts.h"
