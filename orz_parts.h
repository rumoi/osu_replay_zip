#pragma once

// the .osr bytes around the replay data: the header (mode .. mods), the time stamp after the life bar and what follows
// the replay data (online score id, extra data). Stored modelled when they have the usual shape: md5 strings as 16 bytes,
// a replay md5 that a known formula gives back as that formula's index, numbers in buckets. Anything else stays raw.
// The coded life bar (orz_life.h) follows them, to the end of the stream.

#include <array>
#include <bit>
#include <charconv>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "orz_shared.h"
#include "rc.h"

namespace orz {

// unsigned leb128, as .osr strings give their length
inline void add_uleb128(std::vector<u8>& output, u32 v) {

	for (; v > 0x7f; v >>= 7)
		output.push_back(u8(v | 0x80));

	output.push_back(u8(v));
}

namespace osr_parts {

	enum format : u8 {
		format_raw = 0, // each part as a leb128 size and its bytes
		format_fields = 1
	};

	struct _parts {
		std::vector<u8> header, after_life, tail;
		std::vector<u8> life; // coded, empty when the .osr has none
	};

	inline std::array<u8, 16> md5(std::string_view s) {

		static constexpr u32 K[64]{
			0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
			0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
			0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
			0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
			0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
			0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
			0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
			0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
		};

		static constexpr int R[4][4]{ { 7, 12, 17, 22 }, { 5, 9, 14, 20 }, { 4, 11, 16, 23 }, { 6, 10, 15, 21 } };

		std::string m{ s };

		m += '\x80';
		m.append((119 - s.size() % 64) % 64, '\0');

		for (int i{}; i < 8; ++i)
			m += char(((u64)s.size() * 8) >> (8 * i));

		u32 h[4]{ 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };

		for (size_t at{}; at < m.size(); at += 64) {

			u32 w[16];

			for (int i{}; i < 16; ++i)
				w[i] = u32(u8(m[at + i * 4])) | u32(u8(m[at + i * 4 + 1])) << 8 | u32(u8(m[at + i * 4 + 2])) << 16 | u32(u8(m[at + i * 4 + 3])) << 24;

			u32 a{ h[0] }, b{ h[1] }, c{ h[2] }, d{ h[3] };

			for (int i{}; i < 64; ++i) {

				const int r{ i >> 4 };

				const u32 f{ r == 0 ? (b & c) | (~b & d) :
					r == 1 ? (d & b) | (~d & c) :
					r == 2 ? b ^ c ^ d : c ^ (b | ~d) };

				const int g{ r == 0 ? i : r == 1 ? (5 * i + 1) & 15 : r == 2 ? (3 * i + 5) & 15 : (7 * i) & 15 };

				const u32 t{ d };

				d = c;
				c = b;
				b += std::rotl(a + f + K[i] + w[g], R[r][i & 3]);
				a = t;

			}

			h[0] += a;
			h[1] += b;
			h[2] += c;
			h[3] += d;

		}

		std::array<u8, 16> out{};

		for (int i{}; i < 16; ++i)
			out[i] = u8(h[i >> 2] >> (8 * (i & 3)));

		return out;
	}

	// a replay md5 some writers put in the header, as formula index: 0-8 md5(combo "osu" name beatmap_md5 score rank),
	// 9-48 the "p o o t a r e y o u" string with each style of its booleans and rank
	constexpr u32 FORMULAS{ 49 };

	constexpr std::string_view RANKS[]{ "XH", "SH", "X", "S", "A", "B", "C", "D", "F", "0" };

	struct _fields {

		u8 mode;
		i32 version;
		std::string_view beatmap_md5;
		bool has_name;
		std::string_view name;
		std::array<u8, 16> beatmap, replay; // the md5 strings as bytes
		u16 count[6]; // 300, 100, 50, geki, katu, miss
		i32 score;
		u16 combo;
		u8 perfect;
		u32 mods;

	};

	inline std::array<u8, 16> formula_md5(const _fields& f, u32 k) {

		std::string s;

		const auto num = [&](i64 v) {

			char b[24];

			s.append(b, std::to_chars(b, b + sizeof b, v).ptr);

		};

		if (k < 9) {

			num(f.combo);
			s += "osu";
			s += f.name;
			s += f.beatmap_md5;
			num(f.score);
			s += RANKS[k];

			return md5(s);
		}

		k -= 9;

		const bool lower{ ((k / 10) & 1) != 0 };

		num(f.count[0] + f.count[1]);
		s += 'p';
		num(f.count[2]);
		s += 'o';
		num(f.count[3]);
		s += 'o';
		num(f.count[4]);
		s += 't';
		num(f.count[5]);
		s += 'a';
		s += f.beatmap_md5;
		s += 'r';
		num(f.combo);
		s += 'e';
		s += f.perfect ? (lower ? "true" : "True") : (lower ? "false" : "False");
		s += 'y';
		s += f.name;
		s += 'o';
		num(f.score);
		s += 'u';
		s += RANKS[k % 10];
		num((i32)f.mods);
		s += k >= 20 ? "true" : "True";

		return md5(s);
	}

	// 32 lowercase hex digits as 16 bytes
	inline bool from_hex(std::string_view s, std::array<u8, 16>& out) {

		if (s.size() != 32)
			return false;

		for (size_t i{}; i < 32; ++i) {

			const char c{ s[i] };
			const int v{ c >= '0' && c <= '9' ? c - '0' : (c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1) };

			if (v < 0)
				return false;

			out[i >> 1] = u8((out[i >> 1] << 4) | v);

		}

		return true;
	}

	inline std::string to_hex(const std::array<u8, 16>& v) {

		constexpr char HEX[]{ "0123456789abcdef" };

		std::string s(32, '0');

		for (size_t i{}; i < 16; ++i) {

			s[i * 2] = HEX[v[i] >> 4];
			s[i * 2 + 1] = HEX[v[i] & 15];

		}

		return s;
	}

	struct _reader {

		std::span<const u8> b;
		size_t at{};
		bool ok{ true };

		template<typename T>
		T get() {

			T v{};

			if (b.size() - at < sizeof(T)) {

				ok = false;

				return v;
			}

			memcpy(&v, b.data() + at, sizeof(T));
			at += sizeof(T);

			return v;
		}

		// an .osr string: 0x00 for none, else 0x0b, a leb128 length and the bytes
		bool string(bool& present, std::string_view& s) {

			const u8 marker{ get<u8>() };

			present = marker == 0x0b;

			if (!ok || (marker != 0x0b && marker != 0))
				return ok = false;

			if (!present)
				return true;

			u32 n{};

			for (int shift{}; ; shift += 7) {

				const u8 c{ get<u8>() };

				if (!ok || shift > 28)
					return ok = false;

				n |= u32(c & 0x7f) << shift;

				if (!(c & 0x80))
					break;

			}

			if (b.size() - at < n)
				return ok = false;

			s = { (const char*)b.data() + at, n };
			at += n;

			return true;
		}

	};

	inline bool parse(std::span<const u8> header, _fields& f) {

		_reader r{ header };

		f.mode = r.get<u8>();
		f.version = r.get<i32>();

		bool has_beatmap{}, has_replay{};
		std::string_view replay_md5;

		if (!r.string(has_beatmap, f.beatmap_md5) || !r.string(f.has_name, f.name) || !r.string(has_replay, replay_md5))
			return false;

		for (auto& c : f.count)
			c = r.get<u16>();

		f.score = r.get<i32>();
		f.combo = r.get<u16>();
		f.perfect = r.get<u8>();
		f.mods = r.get<u32>();

		return r.ok && r.at == header.size() && has_beatmap && has_replay && f.mode <= 3 && f.perfect <= 1 &&
			from_hex(f.beatmap_md5, f.beatmap) && from_hex(replay_md5, f.replay);
	}

	inline void put_string(std::vector<u8>& o, bool present, std::string_view s) {

		if (!present) {

			o.push_back(0);

			return;
		}

		o.push_back(0x0b);

		add_uleb128(o, (u32)s.size());

		o.insert(o.end(), s.begin(), s.end());

	}

	template<typename T>
	void put(std::vector<u8>& o, T v) {

		const auto i{ o.size() };

		o.resize(i + sizeof(T));

		memcpy(o.data() + i, &v, sizeof(T));

	}

	FORCEINLINE void put_bits(bit_stream& s, u64 v, int n) {

		for (int i{}; i < n; ++i)
			s.push_back((v >> i) & 1);

	}

	constexpr i64 VERSION_BASE{ 20200000 }; // versions are yyyymmdd
	constexpr i64 UNIX_TICKS{ 621355968000000000 }, TICKS_PER_SECOND{ 10000000 };

	FORCEINLINE void put_bytes(bit_stream& s, std::span<const u8> v) {

		for (const u8 c : v)
			put_bits(s, c, 8);

	}

	FORCEINLINE void get_bytes(_bit_view& s, std::span<u8> v) {

		for (auto& c : v)
			c = (u8)s.read_bits<8>();

	}

	// a length, then the bytes
	inline void write_bytes(bit_stream& s, std::span<const u8> v) {

		s.write_bucket((u32)v.size(), 4, 8);
		put_bytes(s, v);

	}

	inline std::vector<u8> read_bytes(_bit_view& s) {

		std::vector<u8> v(std::min<size_t>(s.read_bucket<4, 8>(), s.max_size() / 8));

		get_bytes(s, v);

		return v;
	}

	// online score id: none (a tail shorter than 8 bytes), 0, 32 or 64 bits
	constexpr int ID_BITS[4]{ 0, 0, 32, 64 };

	inline std::vector<u8> encode_fields(const _parts& p) {

		_fields f{};

		if (!parse(p.header, f) || p.after_life.size() != 8 || std::abs(f.version - VERSION_BASE) > (1 << 30))
			return {};

		i64 ticks;

		memcpy(&ticks, p.after_life.data(), 8);

		const i64 seconds{ (ticks - UNIX_TICKS) / TICKS_PER_SECOND }, sub{ (ticks - UNIX_TICKS) % TICKS_PER_SECOND };

		if (ticks < UNIX_TICKS || seconds >= (1ll << 32))
			return {};

		bit_stream s{};

		put_bits(s, f.mode, 2);
		s.write_bucket((u32)rc::zz(f.version - VERSION_BASE), 18);
		put_bits(s, f.has_name, 1);

		if (f.has_name)
			write_bytes(s, { (const u8*)f.name.data(), f.name.size() });

		put_bytes(s, f.beatmap);

		u32 formula{};

		while (formula < FORMULAS && formula_md5(f, formula) != f.replay)
			++formula;

		put_bits(s, formula < FORMULAS, 1);

		if (formula < FORMULAS)
			put_bits(s, formula, 6);
		else
			put_bytes(s, f.replay);

		s.write_bucket(f.count[0], 9, 11);

		for (int k{ 1 }; k < 6; ++k)
			s.write_bucket(f.count[k], 5, 8);

		put_bits(s, (u32)f.score, 32);
		s.write_bucket(f.combo, 9, 11);
		put_bits(s, f.perfect, 1);
		s.write_bucket(f.mods, 16);

		put_bits(s, (u64)seconds, 32);
		put_bits(s, sub != 0, 1);

		if (sub)
			put_bits(s, (u64)sub, 24);

		i64 id{};

		if (p.tail.size() >= 8)
			memcpy(&id, p.tail.data(), 8);

		const u32 id_kind{ p.tail.size() < 8 ? 0u : (id == 0 ? 1u : ((u64)id >> 32 ? 3u : 2u)) };

		put_bits(s, id_kind, 2);
		put_bits(s, (u64)id, ID_BITS[id_kind]);
		write_bytes(s, std::span{ p.tail }.subspan(id_kind ? 8 : 0));

		std::vector<u8> out{ format_fields };

		out.insert(out.end(), s.data.begin(), s.data.end());

		return out;
	}

	inline _parts decode_fields(_bit_view s, size_t& used) {

		_parts p{};
		_fields f{};

		f.mode = (u8)s.read_bits(2);
		f.version = (i32)(VERSION_BASE + rc::unzz(s.read_bucket<18>()));
		f.has_name = s.read_bits(1) != 0;

		const auto name{ f.has_name ? read_bytes(s) : std::vector<u8>{} };

		f.name = { (const char*)name.data(), name.size() };

		get_bytes(s, f.beatmap);

		const std::string beatmap_md5{ to_hex(f.beatmap) };

		f.beatmap_md5 = beatmap_md5;

		const bool derived{ s.read_bits(1) != 0 };
		const u32 formula{ derived ? (u32)s.read_bits(6) : 0u };

		if (!derived)
			get_bytes(s, f.replay);

		f.count[0] = (u16)s.read_bucket<9, 11>();

		for (int k{ 1 }; k < 6; ++k)
			f.count[k] = (u16)s.read_bucket<5, 8>();

		f.score = (i32)s.read_bits(32);
		f.combo = (u16)s.read_bucket<9, 11>();
		f.perfect = (u8)s.read_bits(1);
		f.mods = s.read_bucket<16>();

		if (derived)
			f.replay = formula_md5(f, std::min(formula, FORMULAS - 1));

		auto& h{ p.header };

		h.push_back(f.mode);
		put(h, f.version);
		put_string(h, true, f.beatmap_md5);
		put_string(h, f.has_name, f.name);
		put_string(h, true, to_hex(f.replay));

		for (const u16 c : f.count)
			put(h, c);

		put(h, f.score);
		put(h, f.combo);
		put(h, f.perfect);
		put(h, f.mods);

		const i64 seconds{ (i64)s.read_bits(32) };
		const i64 sub{ s.read_bits(1) ? (i64)s.read_bits(24) : 0 };

		put(p.after_life, UNIX_TICKS + seconds * TICKS_PER_SECOND + sub);

		const u32 id_kind{ (u32)s.read_bits(2) };

		if (id_kind)
			put(p.tail, s.read_bits(ID_BITS[id_kind]));

		const auto rest{ read_bytes(s) };

		p.tail.insert(p.tail.end(), rest.begin(), rest.end());

		used = (s.in + 7) / 8;

		return p;
	}

	inline std::vector<u8> encode_raw(const _parts& p) {

		std::vector<u8> out{ format_raw };

		for (const auto* bytes : { &p.header, &p.after_life, &p.tail }) {

			add_uleb128(out, (u32)bytes->size());

			out.insert(out.end(), bytes->begin(), bytes->end());

		}

		return out;
	}

	inline _parts decode_raw(std::span<const u8> data, size_t& used) {

		_parts p{};

		size_t at{};

		for (auto* part : { &p.header, &p.after_life, &p.tail }) {

			u32 size{};

			for (int shift{}; at < data.size() && shift < 35; shift += 7) {

				size |= u32(data[at] & 0x7f) << shift;

				if (!(data[at++] & 0x80))
					break;

			}

			if (size > data.size() - at)
				break;

			part->assign(data.begin() + at, data.begin() + at + size);
			at += size;

		}

		used = at;

		return p;
	}

	// empty when absent or damaged
	inline _parts decode(std::span<const u8> data) {

		if (data.empty())
			return {};

		size_t used{};

		auto p{ data[0] == format_fields ? decode_fields(_bit_view{ data.subspan(1) }, used) : decode_raw(data.subspan(1), used) };

		if (1 + used < data.size())
			p.life.assign(data.begin() + 1 + used, data.end());

		return p;
	}

	inline std::vector<u8> encode(const _parts& p) {

		auto out{ encode_fields(p) };

		// only kept when it gives back these exact bytes and ends where they do
		if (!out.empty()) {

			const auto back{ decode(out) };

			if (!(back.header == p.header && back.after_life == p.after_life && back.tail == p.tail && back.life.empty()))
				out.clear();

		}

		if (out.empty())
			out = encode_raw(p);

		out.insert(out.end(), p.life.begin(), p.life.end());

		return out;
	}

}

}
