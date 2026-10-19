#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <emmintrin.h>
#include <nmmintrin.h>
#include <fstream>
#include <new>
#include <span>
#include <string>
#include <vector>

// replay data is decoded with the LZMA SDK's decoder (Lib), and encoded the way osu! encodes it (osu_lzma.h)
#include "LzmaDec.h"
#include "orz_types.h"
#include "osu_lzma.h"

namespace orz {

#if defined(_M_FP_FAST) || defined(_M_FP_CONTRACT) || defined(__FAST_MATH__)
#error "breaks without strict IEEE float maths (/fp:precise)"
#endif
#if defined(_M_IX86) && _M_IX86_FP < 2
#error "needs SSE2 floating point (/arch:SSE2)"
#endif
static_assert(FLT_EVAL_METHOD == 0, "float must not use extended precision (x87)");

#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#if !defined(ORZ_FP_CONTRACT_OFF)
#error "GCC fuses a * b + c by default: build with -ffp-contract=off and define ORZ_FP_CONTRACT_OFF"
#endif
#endif

#if defined(_MSC_VER)
#define FORCEINLINE __forceinline
#define TARGET_SSE42
#else
#define FORCEINLINE inline __attribute__((always_inline))
#define TARGET_SSE42 __attribute__((target("sse4.2"))) // the CRC instructions, without building everything for SSE 4.2
#endif

// MSVC generates terrible code for std::bit_cast<float>(u32) - have to use this
FORCEINLINE float float_from_bits(u32 bits) {
	return _mm_cvtss_f32(_mm_castsi128_ps(_mm_cvtsi32_si128((int)bits)));
}

// same float bits (tells -0 from 0, NaNs apart)
FORCEINLINE bool same_bits(float a, float b) {
	return std::bit_cast<u32>(a) == std::bit_cast<u32>(b);
}

inline void no_FMA_check() {

	static const bool res{ []() {

		volatile float a{ 1.f + 0x1p-12f }, c{ 1.f };
		volatile double x{ 1.0 + 0x1p-27 }, z{ 1.0 };

		const float f{ a * a - c };
		const double d{ x * x - z };

		return (f == 0x1p-11f && d == 0x1p-26);
	}() };

	if (!res) {

		fputs("orz: this build fuses a * b + c into MA, its floats differ from the format's (build with -ffp-contract=off)\n", stderr);
		std::abort();

	}

}

constexpr u32 ORZ_VERSION{ 0 };

// limits for untrusted input, real replays are far below all of them and anything beyond is treated as corrupted
constexpr size_t ORZ_MAX_FRAMES{ 1u << 22 }; // (~19 hours at 60 fps)
constexpr size_t ORZ_MAX_STREAM{ 1u << 28 };

// extended game mode: the game mode with the gameplay modifiers that change it completely (relax, autopilot), and so the
// form of its frames. osu!catch relax is coded as osu!standard frames (the cursor drives the catcher), as are osu!mania
// replays that do not fit the column form (stored as osu!). The first four are the .osr's game mode numbers.
enum class e_mode : u32 { osu, taiko, fruits, mania, osu_relax, taiko_relax, fruits_relax, osu_autopilot };

// the encode info as a file stores it: the e_mode in the low 3 bits, then a bit each
enum ORZ_ENCODE_INFO : u32 {

	e_mode_bits = 7,

	has_osr_header = 1 << 3,

	trailing_comma = 1 << 4,

	replay_verbatim = 1 << 5,

	text_shortest = 1 << 6, // floats are 'shortest round-trip' (newer .NET, osu!lazer)

};

constexpr u32 ORZ_ENCODE_INFO_BITS{ 7 };

FORCEINLINE e_mode get_e_mode(u32 encode_info) {
	return (e_mode)(encode_info & ORZ_ENCODE_INFO::e_mode_bits);
}

FORCEINLINE u32 set_e_mode(u32 encode_info, e_mode m) {
	return (encode_info & ~u32(ORZ_ENCODE_INFO::e_mode_bits)) | (u32)m;
}

// the game mode, numbered as the .osr does (osu!, taiko, catch, mania)
FORCEINLINE u32 game_mode(e_mode m) {
	return m == e_mode::osu_autopilot ? 0 : (u32)m & 3;
}

FORCEINLINE bool is_relax(e_mode m) {
	return m >= e_mode::osu_relax && m <= e_mode::fruits_relax;
}

// frames coded by the other modes' coder (orz_modes.h), else osu!standard's (orz_frames.h)
FORCEINLINE bool uses_modes_coder(e_mode m) {
	return m == e_mode::taiko || m == e_mode::fruits || m == e_mode::mania || m == e_mode::taiko_relax;
}

enum ORZ_OPTION : u32 {

	ENCODE_CHECK_ROUND_TRIP = 1 << 0,

	DECODE_SKIP_CRC = 1 << 2,

};

constexpr u32 OSR_MOD_RELAX{ 1 << 7 };

enum class orz_tag : u8 {

	// delta table and the mode's model, then the context mixed frames: orz_frames.h (osu!standard), orz_modes.h (other modes)
	frames = 1,

	osr_meta_data, // the .osr bytes around the replay data: the header, the life bar, what follows it, what follows the replay data (orz_parts.h)
	osr_verbatim,

};

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

	bool emplace_back(const bool v) {

		if (in_count > 7 || data.size() == 0) {

			in_count = 0;
			data.push_back({});

		}

		data.back() |= u8(v) << (in_count++);

		return v;
	}

	void push_back(const bool v) {
		emplace_back(v);
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

	template<u32 OVERFLOW_BITS = 5, typename ...T >
	void write_bucket(u32 value, const T... bits) {

		bool done{};

		const auto write = [&](const size_t b) {

			if (b == 0 || done)
				return;

			const u32 MAX_VALUE{ (1u << b) - 1 };

			if (value < MAX_VALUE) {

				for (size_t i{}; i < b; ++i)
					push_back((value >> i) & 1);

				done = true;

				return;
			}

			for (size_t i{}; i < b; ++i)
				push_back(1);

			value -= MAX_VALUE;

		};

		(write(bits), ...);

		if (done)
			return;

		value += 1;

		const auto in{ (size_t)31 - std::countl_zero(value) };

		for (size_t i{}; i < OVERFLOW_BITS; ++i)
			push_back((in >> i) & 1);

		for (size_t i{}; i < in; ++i)
			push_back((value >> i) & 1);

	}

	template<size_t BIT_WRITE>
	void write_bits(u64 value) {

		for (size_t i{}; i < BIT_WRITE; ++i)
			push_back((value >> i) & 1ull);

	}

	template<size_t BIT_WRITE>
	void small_write(u32 value) {

		constexpr u32 MAX_VALUE{ (1u << BIT_WRITE) - 1 };

		if (value < MAX_VALUE)
			return write_bits<BIT_WRITE>(value);

		const int in{ 31 - std::countl_zero(value) };

		for (int i{}; i <= in; ++i)
			push_back(1);

		push_back(0);

		for (int i{}; i < in; ++i)
			push_back((value >> i) & 1);

	}

	inline void write_float(const float v) {
		write_bits<32>(std::bit_cast<u32>(v));
	}
	
	template<u32 LENGTH_BITS, u32 FROM>
	void write_length_bucket(u32 value) {

		constexpr u32 ESCAPE{ (1u << LENGTH_BITS) - 1 };
		constexpr u32 REST_BITS{ (u32)std::bit_width(32u - ESCAPE) };

		static_assert(FROM + ESCAPE <= 33);

		const u32 length{ (u32)std::bit_width(value) };
		const u32 code{ std::min(length - FROM, ESCAPE) };

		write_bits<LENGTH_BITS>(code);

		if (code == ESCAPE)
			write_bits<REST_BITS>(length < FROM ? length : length - ESCAPE);

		for (u32 k{}; k + 1 < length; ++k)
			push_back((value >> k) & 1);

	}



};

struct _bit_view {

	std::span<const u8> data;
	size_t in{};

	_bit_view(std::span<const u8> bytes) : data{ bytes } {}
	_bit_view(const bit_stream& bits) : data{ bits.data } {}

	std::span<const u8> get_tail() const {
		return data.subspan(std::min((in + 7) / 8, data.size()));
	}

	size_t max_size() const {
		return data.size() * 8;
	}

	bool operator[](size_t i) const {

		const size_t bucket{ i >> 3 };

		return bucket < data.size() && ((data[bucket] >> (i & 7)) & 1);
	}

	template<size_t BIT_WRITE>
	u64 read_bits() {

		u64 ret{};
		
		for (size_t i{}; i < BIT_WRITE; ++i)
			ret |= u64(operator[](in++)) << i;

		return ret;
	}

	u64 read_bits(const size_t count) {

		u64 ret{};

		for (size_t i{}; i < count; ++i)
			ret |= u64(operator[](in++)) << i;

		return ret;
	}



	template<u32... BITS>
	u32 read_bucket() {

		constexpr size_t OVERFLOW_BITS{ 5 };

		u32 result{};

		const auto part = [&](auto bits) {

			constexpr u32 B{ decltype(bits)::value };

			if constexpr (B == 0)
				return false;
			else {

				const u32 v{ (u32)read_bits<B>() };

				result += v;

				return v != (1u << B) - 1;
			}
		};

		if ((part(std::integral_constant<u32, BITS>{}) || ...))
			return result;

		const u32 bit_count{ (u32)read_bits<OVERFLOW_BITS>() };

		const u32 v{ (u32)(read_bits(bit_count) | (u64(1) << bit_count)) };		

		return result + v - 1;
	}

	template<u32 LENGTH_BITS, u32 FROM>
	u64 read_length_bucket() {

		constexpr u32 ESCAPE{ (1u << LENGTH_BITS) - 1 };
		constexpr u32 REST_BITS{ (u32)std::bit_width(32u - ESCAPE) };

		const u32 code{ (u32)read_bits<LENGTH_BITS>() };

		u32 length{ code + FROM };

		if (code == ESCAPE) {

			const u32 rest{ (u32)read_bits<REST_BITS>() };

			length = std::min(rest < FROM ? rest : rest + ESCAPE, 32u);

		}

		u64 value{ length ? 1ull << (length - 1) : 0ull };

		for (u32 k{}; k + 1 < length; ++k)
			value |= u64(read_bits<1>()) << k;

		return value;
	}

	template<size_t BIT_WRITE>
	u32 small_read() {

		constexpr u32 MAX_VALUE{ (1u << BIT_WRITE) - 1 };

		const u32 value{ (u32)read_bits<BIT_WRITE>() };

		if (value < MAX_VALUE)
			return value;

		u32 bit_count{ BIT_WRITE - 1 };

		while (read_bits<1>())
			++bit_count;

		bit_count = std::min(bit_count, 31u);

		return (1u << bit_count) | (u32)read_bits(bit_count);
	}

	float read_float() {
		return float_from_bits((u32)read_bits<32>());
	}


};

constexpr u8 ORZ_MAGIC{ 0xE6 };

// tuned to sessions under around 4.7 hours
constexpr u32 ORZ_SEED_LENGTH{ 18 };

template<size_t str_size>
struct small_string{

	u8 data[str_size + 1];

	const size_t size() const noexcept {
		return std::min(str_size, (size_t)data[str_size]);
	}

	void set_size(size_t i) noexcept {

		data[str_size] = i;
		data[str_size] = size();

	}


	std::string_view to_str_view() const {

		return std::string_view{ (const char*)data, (size_t)size() };
	}

	std::span<u8> to_span() {

		return std::span<u8>{ data, (size_t)size() };
	}

	u8* begin() { return data; }
	const u8* begin() const { return data; }

	u8* end() { return data + size(); }
	const u8* end() const { return data + size(); }


};


struct _orz_file {

	u32 version;
	u32 CRC;
	u32 encode_info;

	u32 seeded;
	u32 seed;

	u32 frame_count;

	struct _container {

		std::vector<u8> data;
		orz_tag tag;

		auto operator<=>(const _container& o) const {
			return tag <=> o.tag;
		}

		bool operator==(const _container& o) const {
			return tag == o.tag;
		}

	};

	struct _container_custom {

		small_string<31> name; // SPEC: name can't use extended ASCII
		std::vector<u8> data;

	};

	std::vector<_container> container;

	std::vector<_container_custom> container_custom;

	bool decode(const std::span<const u8> bytes) {

		container.clear();
		container_custom.clear();

		_bit_view bs{ bytes };

		if (bs.read_bits<8>() != ORZ_MAGIC)
			return 0;

		version = bs.read_bucket<4, 8>();

		if (version > ORZ_VERSION) // we are trying to read a file from the future, abort.
			return 0;

		CRC = bs.read_bits<32>();

		encode_info = bs.read_bucket<7>();
		
		if (seeded = bs.read_bits<1>())
			seed = bs.read_length_bucket<3, ORZ_SEED_LENGTH>();

		frame_count = bs.read_bucket<16>();

		{

			orz_tag last_tag{};

			while (bs.read_bits<1>()) {

				_container& r{ container.emplace_back() };

				u64 tag{ (u64)last_tag + 1 };

				if (bs.read_bits<1>())
					tag += bs.read_bucket<4>();

				if (tag > 255)
					return 0;

				r.tag = orz_tag(tag);

				const auto container_size{ bs.read_bucket<16>() };

				if (container_size > ORZ_MAX_STREAM || container_size > bytes.size())
					return 0;

				r.data.resize(container_size);

				last_tag = r.tag;

			}

		}

		if (bs.read_bits<1>()) {

			const u64 custom_count{ 1ull + bs.read_bucket<4>() };
			
			if (custom_count > bytes.size() * 8 / 21)
				return 0;

			container_custom.resize(custom_count);

		}

		for (auto& con : container_custom) {

			con.name.set_size(bs.read_bits<5>());

			for (auto& c : con.name.to_span())
				c = bs.read_bits<7>();

			const auto container_size{ bs.read_bucket<16>() };

			if (container_size > ORZ_MAX_STREAM || container_size > bytes.size())
				return 0;

			con.data.resize(container_size);

		}

		if (bs.in > bs.max_size())
			return 0;

		auto data_block{ bs.get_tail() };

		const auto fill = [&](auto& list) {

			for (auto& c : list) {

				const size_t size{ c.data.size() };

				if (size > data_block.size())
					return 0;

				const auto part{ data_block.first(size) };

				c.data.assign(part.begin(), part.end());

				data_block = data_block.subspan(size);

			}

			return 1;
		};

		if (fill(container) == 0 || fill(container_custom) == 0)
			return 0;

		return 1;
	}

	bool encode(std::vector<u8>& output_orz) {

		output_orz.clear();
		
		for (const auto& con : container_custom)
			for (const u8 c : con.name)
				if (c > 127)
					return 0;

		bit_stream bs{};

		bs.write_bits<8>(ORZ_MAGIC);

		bs.write_bucket(version, 4, 8);

		bs.write_bits<32>(CRC);

		bs.write_bucket(encode_info, 7);
		
		if (bs.emplace_back(seeded))
			bs.write_length_bucket<3, ORZ_SEED_LENGTH>(seed);

		bs.write_bucket(frame_count, 16);

		std::ranges::sort(container);

		orz_tag last_tag{};

		for (const auto& con : container) {

			const auto delta = u8((u8)con.tag - (u8)last_tag);

			if (delta == 0) // silently ignoring duplicate tags
				continue;

			bs.write_bits<1>(1);

			if (bs.emplace_back(delta != 1))
				bs.write_bucket(delta - 1, 4);

			bs.write_bucket(con.data.size(), 16);
			add_to_u8(output_orz, con.data);

			last_tag = con.tag;

		}

		bs.write_bits<1>(0);

		{

			const auto custom_count{ container_custom.size() };

			if (bs.emplace_back(custom_count > 0))
				bs.write_bucket(custom_count - 1, 4);

			for (const auto& con : container_custom) {

				bs.write_bits<5>(con.name.size());

				for (const auto& c : con.name)
					bs.write_bits<7>(c);

				bs.write_bucket(con.data.size(), 16);
				add_to_u8(output_orz, con.data);

			}

		}

		std::swap(bs.data, output_orz);
		
		add_to_u8(output_orz, bs.data);

		return 1;
	}

};

inline bool is_newer_orz(std::span<const u8> file) {

	_bit_view bs{ file };

	return bs.read_bits<8>() == ORZ_MAGIC && bs.read_bucket<4, 8>() > ORZ_VERSION;
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

// the containers the CRC covers after the frames, in this order (the frames container is covered by the decoded frames)
constexpr orz_tag CRC_PART_TAGS[]{
	orz_tag::osr_meta_data,
	orz_tag::osr_verbatim
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

}
