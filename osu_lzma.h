#pragma once

// osu!stable's replay compression: the C# LZMA encoder from the LZMA SDK (public domain, by Igor Pavlov) with the
// settings osu! uses: a 2 MiB dictionary, lc 3, lp 0, pb 2, 255 fast bytes, the BT4 match finder and no end marker.
// The SDK's C encoder makes other choices since 4.58, so only this encoder gives back the bytes osu! wrote.
// Ported line for line, including the input window the C# match finder reads through, since it decides what the
// match finder sees. Unsigned arithmetic wraps as it does in C#.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

#include "orz_types.h"

namespace orz {

namespace osu_lzma {

	constexpr u32 DICTIONARY_SIZE{ 1u << 21 }, NUM_FAST_BYTES{ 255 };
	constexpr int LIT_CONTEXT_BITS{ 3 }, LIT_POS_BITS{ 0 }, POS_STATE_BITS{ 2 };

	constexpr u32 NUM_REP_DISTANCES{ 4 }, NUM_STATES{ 12 };
	constexpr int NUM_POS_SLOT_BITS{ 6 }, NUM_LEN_TO_POS_STATES_BITS{ 2 };
	constexpr u32 NUM_LEN_TO_POS_STATES{ 1u << NUM_LEN_TO_POS_STATES_BITS };
	constexpr u32 MATCH_MIN_LEN{ 2 };
	constexpr int NUM_ALIGN_BITS{ 4 };
	constexpr u32 ALIGN_TABLE_SIZE{ 1u << NUM_ALIGN_BITS }, ALIGN_MASK{ ALIGN_TABLE_SIZE - 1 };
	constexpr u32 START_POS_MODEL_INDEX{ 4 }, END_POS_MODEL_INDEX{ 14 };
	constexpr u32 NUM_FULL_DISTANCES{ 1u << (END_POS_MODEL_INDEX / 2) };
	constexpr int NUM_POS_STATES_BITS_MAX{ 4 };
	constexpr u32 NUM_POS_STATES_ENCODING_MAX{ 1u << 4 };
	constexpr int NUM_LOW_LEN_BITS{ 3 }, NUM_MID_LEN_BITS{ 3 }, NUM_HIGH_LEN_BITS{ 8 };
	constexpr u32 NUM_LOW_LEN_SYMBOLS{ 1u << NUM_LOW_LEN_BITS }, NUM_MID_LEN_SYMBOLS{ 1u << NUM_MID_LEN_BITS };
	constexpr u32 NUM_LEN_SYMBOLS{ NUM_LOW_LEN_SYMBOLS + NUM_MID_LEN_SYMBOLS + (1u << NUM_HIGH_LEN_BITS) };
	constexpr u32 MATCH_MAX_LEN{ MATCH_MIN_LEN + NUM_LEN_SYMBOLS - 1 };
	constexpr u32 NUM_OPTS{ 1u << 12 };
	constexpr u32 INFINITY_PRICE{ 0xFFFFFFF };

	inline u32 len_to_pos_state(u32 len) {

		len -= MATCH_MIN_LEN;

		return len < NUM_LEN_TO_POS_STATES ? len : NUM_LEN_TO_POS_STATES - 1;
	}

	struct _state {

		u32 index{};

		void update_char() { index = index < 4 ? 0 : index < 10 ? index - 3 : index - 6; }
		void update_match() { index = index < 7 ? 7 : 10; }
		void update_rep() { index = index < 7 ? 8 : 11; }
		void update_short_rep() { index = index < 7 ? 9 : 11; }
		bool is_char() const { return index < 7; }
	};

	struct _tables {

		std::array<u32, 256> crc{};
		std::array<u8, 1 << 11> fast_pos{};
		std::array<u32, (2048 >> 2)> prob_prices{};

		_tables() {

			for (u32 i{}; i < 256; ++i) {

				u32 r{ i };

				for (int j{}; j < 8; ++j)
					r = (r & 1) ? (r >> 1) ^ 0xEDB88320u : r >> 1;

				crc[i] = r;

			}

			fast_pos[0] = 0;
			fast_pos[1] = 1;

			for (u32 slot{ 2 }, c{ 2 }; slot < 22; ++slot)
				for (u32 j{}, k{ 1u << ((slot >> 1) - 1) }; j < k; ++j, ++c)
					fast_pos[c] = (u8)slot;

			constexpr int BITS{ 11 - 2 };

			for (int i{ BITS - 1 }; i >= 0; --i) {

				const u32 start{ 1u << (BITS - i - 1) }, end{ 1u << (BITS - i) };

				for (u32 j{ start }; j < end; ++j)
					prob_prices[j] = ((u32)i << 6) + (((end - j) << 6) >> (BITS - i - 1));

			}

		}
	};

	inline const _tables& tables() {

		static const _tables t{};

		return t;
	}

	inline u32 pos_slot(u32 pos) {

		const auto& f{ tables().fast_pos };

		if (pos < (1u << 11))
			return f[pos];

		if (pos < (1u << 21))
			return f[pos >> 10] + 20u;

		return f[pos >> 20] + 40u;
	}

	inline u32 pos_slot2(u32 pos) {

		const auto& f{ tables().fast_pos };

		if (pos < (1u << 17))
			return f[pos >> 6] + 12u;

		if (pos < (1u << 27))
			return f[pos >> 16] + 32u;

		return f[pos >> 26] + 52u;
	}

	struct _range_encoder {

		std::vector<u8>* out{};
		u64 low{};
		u32 range{ 0xFFFFFFFFu }, cache_size{ 1 };
		u8 cache{};

		void shift_low() {

			if ((u32)low < 0xFF000000u || (u32)(low >> 32) == 1) {

				u8 temp{ cache };

				do {
					out->push_back((u8)(temp + (low >> 32)));
					temp = 0xFF;
				} while (--cache_size != 0);

				cache = (u8)((u32)low >> 24);

			}

			++cache_size;
			low = (u64)((u32)low << 8);

		}

		void encode_direct_bits(u32 v, int bits) {

			for (int i{ bits - 1 }; i >= 0; --i) {

				range >>= 1;

				if (((v >> i) & 1) == 1)
					low += range;

				if (range < (1u << 24)) {

					range <<= 8;
					shift_low();

				}

			}

		}

		void flush() {

			for (int i{}; i < 5; ++i)
				shift_low();

		}
	};

	struct _bit {

		u32 prob{};

		void init() { prob = 2048 >> 1; }

		void encode(_range_encoder& rc, u32 symbol) {

			const u32 bound{ (rc.range >> 11) * prob };

			if (symbol == 0) {

				rc.range = bound;
				prob += (2048 - prob) >> 5;

			} else {

				rc.low += bound;
				rc.range -= bound;
				prob -= prob >> 5;

			}

			if (rc.range < (1u << 24)) {

				rc.range <<= 8;
				rc.shift_low();

			}

		}

		u32 price(u32 symbol) const { return tables().prob_prices[(((prob - symbol) ^ (0u - symbol)) & 2047) >> 2]; }
		u32 price0() const { return tables().prob_prices[prob >> 2]; }
		u32 price1() const { return tables().prob_prices[(2048 - prob) >> 2]; }
	};

	struct _bit_tree {

		std::vector<_bit> models;
		int levels{};

		explicit _bit_tree(int bits = 0) : models(size_t(1) << bits), levels{ bits } {}

		void init() {

			for (u32 i{ 1 }; i < (1u << levels); ++i)
				models[i].init();

		}

		void encode(_range_encoder& rc, u32 symbol) {

			for (u32 m{ 1 }, bit_index{ (u32)levels }; bit_index > 0;) {

				--bit_index;

				const u32 bit{ (symbol >> bit_index) & 1 };

				models[m].encode(rc, bit);
				m = (m << 1) | bit;

			}

		}

		void reverse_encode(_range_encoder& rc, u32 symbol) {

			for (u32 m{ 1 }, i{}; i < (u32)levels; ++i) {

				const u32 bit{ symbol & 1 };

				models[m].encode(rc, bit);
				m = (m << 1) | bit;
				symbol >>= 1;

			}

		}

		u32 price(u32 symbol) const {

			u32 p{}, m{ 1 };

			for (int bit_index{ levels }; bit_index > 0;) {

				--bit_index;

				const u32 bit{ (symbol >> bit_index) & 1 };

				p += models[m].price(bit);
				m = (m << 1) + bit;

			}

			return p;
		}

		u32 reverse_price(u32 symbol) const { return reverse_price(models.data(), 0, levels, symbol); }

		static u32 reverse_price(const _bit* models, u32 start, int levels, u32 symbol) {

			u32 p{}, m{ 1 };

			for (int i{ levels }; i > 0; --i) {

				const u32 bit{ symbol & 1 };

				symbol >>= 1;
				p += models[start + m].price(bit);
				m = (m << 1) | bit;

			}

			return p;
		}

		static void reverse_encode(_bit* models, u32 start, _range_encoder& rc, int levels, u32 symbol) {

			for (u32 m{ 1 }, i{}; i < (u32)levels; ++i) {

				const u32 bit{ symbol & 1 };

				models[start + m].encode(rc, bit);
				m = (m << 1) | bit;
				symbol >>= 1;

			}

		}
	};

	struct _literal_coder {

		std::array<_bit, 0x300> m{};

		void init() {

			for (auto& b : m)
				b.init();

		}

		void encode(_range_encoder& rc, u8 symbol) {

			for (u32 context{ 1 }, i{ 8 }; i-- > 0;) {

				const u32 bit{ (u32)(symbol >> i) & 1 };

				m[context].encode(rc, bit);
				context = (context << 1) | bit;

			}

		}

		void encode_matched(_range_encoder& rc, u8 match_byte, u8 symbol) {

			bool same{ true };

			for (u32 context{ 1 }, i{ 8 }; i-- > 0;) {

				const u32 bit{ (u32)(symbol >> i) & 1 };
				u32 state{ context };

				if (same) {

					const u32 match_bit{ (u32)(match_byte >> i) & 1 };

					state += (1 + match_bit) << 8;
					same = match_bit == bit;

				}

				m[state].encode(rc, bit);
				context = (context << 1) | bit;

			}

		}

		u32 price(bool match_mode, u8 match_byte, u8 symbol) const {

			u32 p{}, context{ 1 };
			int i{ 7 };

			if (match_mode) {

				for (; i >= 0; --i) {

					const u32 match_bit{ (u32)(match_byte >> i) & 1 }, bit{ (u32)(symbol >> i) & 1 };

					p += m[((1 + match_bit) << 8) + context].price(bit);
					context = (context << 1) | bit;

					if (match_bit != bit) {

						--i;

						break;
					}

				}

			}

			for (; i >= 0; --i) {

				const u32 bit{ (u32)(symbol >> i) & 1 };

				p += m[context].price(bit);
				context = (context << 1) | bit;

			}

			return p;
		}
	};

	struct _len_encoder {

		_bit choice, choice2;
		std::vector<_bit_tree> low, mid;
		_bit_tree high{ NUM_HIGH_LEN_BITS };

		std::vector<u32> prices = std::vector<u32>(NUM_LEN_SYMBOLS << 4);
		std::array<u32, NUM_POS_STATES_ENCODING_MAX> counters{};
		u32 table_size{};

		_len_encoder() {

			for (u32 s{}; s < NUM_POS_STATES_ENCODING_MAX; ++s) {

				low.emplace_back(NUM_LOW_LEN_BITS);
				mid.emplace_back(NUM_MID_LEN_BITS);

			}

		}

		void init(u32 num_pos_states) {

			choice.init();
			choice2.init();

			for (u32 s{}; s < num_pos_states; ++s) {

				low[s].init();
				mid[s].init();

			}

			high.init();

		}

		void encode_symbol(_range_encoder& rc, u32 symbol, u32 pos_state) {

			if (symbol < NUM_LOW_LEN_SYMBOLS) {

				choice.encode(rc, 0);
				low[pos_state].encode(rc, symbol);

			} else {

				symbol -= NUM_LOW_LEN_SYMBOLS;
				choice.encode(rc, 1);

				if (symbol < NUM_MID_LEN_SYMBOLS) {

					choice2.encode(rc, 0);
					mid[pos_state].encode(rc, symbol);

				} else {

					choice2.encode(rc, 1);
					high.encode(rc, symbol - NUM_MID_LEN_SYMBOLS);

				}

			}

		}

		void set_prices(u32 pos_state, u32 num_symbols, u32 st) {

			const u32 a0{ choice.price0() }, a1{ choice.price1() };
			const u32 b0{ a1 + choice2.price0() }, b1{ a1 + choice2.price1() };

			u32 i{};

			for (; i < NUM_LOW_LEN_SYMBOLS; ++i) {

				if (i >= num_symbols)
					return;

				prices[st + i] = a0 + low[pos_state].price(i);

			}

			for (; i < NUM_LOW_LEN_SYMBOLS + NUM_MID_LEN_SYMBOLS; ++i) {

				if (i >= num_symbols)
					return;

				prices[st + i] = b0 + mid[pos_state].price(i - NUM_LOW_LEN_SYMBOLS);

			}

			for (; i < num_symbols; ++i)
				prices[st + i] = b1 + high.price(i - NUM_LOW_LEN_SYMBOLS - NUM_MID_LEN_SYMBOLS);

		}

		u32 price(u32 symbol, u32 pos_state) const { return prices[pos_state * NUM_LEN_SYMBOLS + symbol]; }

		void update_table(u32 pos_state) {

			set_prices(pos_state, table_size, pos_state * NUM_LEN_SYMBOLS);
			counters[pos_state] = table_size;

		}

		void update_tables(u32 num_pos_states) {

			for (u32 s{}; s < num_pos_states; ++s)
				update_table(s);

		}

		void encode(_range_encoder& rc, u32 symbol, u32 pos_state) {

			encode_symbol(rc, symbol, pos_state);

			if (--counters[pos_state] == 0)
				update_table(pos_state);

		}
	};

	// the input window and BT4 binary tree match finder, reading the input as a MemoryStream would
	struct _match_finder {

		static constexpr u32 HASH2_SIZE{ 1u << 10 }, HASH3_SIZE{ 1u << 16 }, HASH3_OFFSET{ HASH2_SIZE };
		static constexpr u32 FIX_HASH_SIZE{ HASH2_SIZE + HASH3_SIZE }, START_MAX_LEN{ 1 }, MIN_MATCH_CHECK{ 4 };
		static constexpr u32 MAX_VAL_FOR_NORMALIZE{ (1u << 31) - 1 };

		std::span<const u8> input;
		size_t read{};

		std::vector<u8> buffer;
		u32 pos_limit{}, last_safe{}, buffer_offset{}, block_size{}, pos{}, keep_before{}, keep_after{}, stream_pos{};
		bool stream_end{};

		std::vector<u32> son, hash;
		u32 cyclic_pos{}, cyclic_size{}, match_max_len{}, cut_value{}, hash_mask{};

		void create(u32 history, u32 keep_add_before, u32 max_len, u32 keep_add_after) {

			cut_value = 16 + (max_len >> 1);

			const u32 reserve{ (history + keep_add_before + max_len + keep_add_after) / 2 + 256 };

			keep_before = history + keep_add_before;
			keep_after = max_len + keep_add_after;
			block_size = keep_before + keep_after + reserve;
			buffer.assign(block_size, 0);
			last_safe = block_size - keep_after;

			match_max_len = max_len;
			cyclic_size = history + 1;
			son.assign((size_t)cyclic_size * 2, 0);

			u32 hs{ history - 1 };

			hs |= hs >> 1;
			hs |= hs >> 2;
			hs |= hs >> 4;
			hs |= hs >> 8;
			hs >>= 1;
			hs |= 0xFFFF;

			if (hs > (1u << 24))
				hs >>= 1;

			hash_mask = hs;
			hash.assign((size_t)hs + 1 + FIX_HASH_SIZE, 0);

		}

		void read_block() {

			if (stream_end)
				return;

			while (true) {

				const i32 size{ (i32)(0u - buffer_offset + block_size - stream_pos) };

				if (size == 0)
					return;

				const size_t n{ std::min<size_t>((size_t)size, input.size() - read) };

				memcpy(buffer.data() + (u32)(buffer_offset + stream_pos), input.data() + read, n);
				read += n;

				if (n == 0) {

					pos_limit = stream_pos;

					if (buffer_offset + pos_limit > last_safe)
						pos_limit = last_safe - buffer_offset;

					stream_end = true;

					return;
				}

				stream_pos += (u32)n;

				if (stream_pos >= pos + keep_after)
					pos_limit = stream_pos - keep_after;

			}

		}

		void move_block() {

			u32 offset{ buffer_offset + pos - keep_before };

			// one more byte, since move_pos moves by 1
			if (offset > 0)
				--offset;

			const u32 n{ buffer_offset + stream_pos - offset };

			memmove(buffer.data(), buffer.data() + offset, n);
			buffer_offset -= offset;

		}

		void reduce_offsets(i32 sub) {

			buffer_offset += (u32)sub;
			pos_limit -= (u32)sub;
			pos -= (u32)sub;
			stream_pos -= (u32)sub;

		}

		void init() {

			buffer_offset = 0;
			pos = 0;
			stream_pos = 0;
			stream_end = false;
			read_block();

			std::fill(hash.begin(), hash.end(), 0u);
			cyclic_pos = 0;
			reduce_offsets(-1);

		}

		void normalize_links(std::vector<u32>& items, u32 sub) {

			for (auto& v : items)
				v = v <= sub ? 0 : v - sub;

		}

		void move_pos() {

			if (++cyclic_pos >= cyclic_size)
				cyclic_pos = 0;

			++pos;

			if (pos > pos_limit) {

				if (buffer_offset + pos > last_safe)
					move_block();

				read_block();

			}

			if (pos == MAX_VAL_FOR_NORMALIZE) {

				const u32 sub{ pos - cyclic_size };

				normalize_links(son, sub);
				normalize_links(hash, sub);
				reduce_offsets((i32)sub);

			}

		}

		u8 index_byte(i32 index) const { return buffer[(u32)(buffer_offset + pos + (u32)index)]; }

		u32 match_len(i32 index, u32 distance, u32 limit) const {

			if (stream_end && (i64)pos + index + limit > (i64)stream_pos)
				limit = stream_pos - (u32)((i64)pos + index);

			++distance;

			const u32 p{ buffer_offset + pos + (u32)index };

			u32 i{};

			while (i < limit && buffer[p + i] == buffer[p + i - distance])
				++i;

			return i;
		}

		u32 available() const { return stream_pos - pos; }

		u32 get_matches(u32* distances) {

			u32 len_limit;

			if (pos + match_max_len <= stream_pos)
				len_limit = match_max_len;
			else {

				len_limit = stream_pos - pos;

				if (len_limit < MIN_MATCH_CHECK) {

					move_pos();

					return 0;
				}

			}

			const auto& crc{ tables().crc };

			u32 offset{};
			const u32 match_min_pos{ pos > cyclic_size ? pos - cyclic_size : 0 };
			const u32 cur{ buffer_offset + pos };
			u32 max_len{ START_MAX_LEN };

			u32 temp{ crc[buffer[cur]] ^ buffer[cur + 1] };
			const u32 hash2{ temp & (HASH2_SIZE - 1) };

			temp ^= (u32)buffer[cur + 2] << 8;

			const u32 hash3{ temp & (HASH3_SIZE - 1) };
			const u32 hash_value{ (temp ^ (crc[buffer[cur + 3]] << 5)) & hash_mask };

			u32 cur_match{ hash[FIX_HASH_SIZE + hash_value] };
			u32 cur_match2{ hash[hash2] };
			const u32 cur_match3{ hash[HASH3_OFFSET + hash3] };

			hash[hash2] = pos;
			hash[HASH3_OFFSET + hash3] = pos;

			if (cur_match2 > match_min_pos && buffer[buffer_offset + cur_match2] == buffer[cur]) {

				distances[offset++] = max_len = 2;
				distances[offset++] = pos - cur_match2 - 1;

			}

			if (cur_match3 > match_min_pos && buffer[buffer_offset + cur_match3] == buffer[cur]) {

				if (cur_match3 == cur_match2)
					offset -= 2;

				distances[offset++] = max_len = 3;
				distances[offset++] = pos - cur_match3 - 1;
				cur_match2 = cur_match3;

			}

			if (offset != 0 && cur_match2 == cur_match) {

				offset -= 2;
				max_len = START_MAX_LEN;

			}

			hash[FIX_HASH_SIZE + hash_value] = pos;

			u32 ptr0{ (cyclic_pos << 1) + 1 }, ptr1{ cyclic_pos << 1 };
			u32 len0{}, len1{};

			for (u32 count{ cut_value }; ; ) {

				if (cur_match <= match_min_pos || count-- == 0) {

					son[ptr0] = son[ptr1] = 0;

					break;
				}

				const u32 delta{ pos - cur_match };
				const u32 cyclic{ (delta <= cyclic_pos ? cyclic_pos - delta : cyclic_pos - delta + cyclic_size) << 1 };
				const u32 pby1{ buffer_offset + cur_match };

				u32 len{ std::min(len0, len1) };

				if (buffer[pby1 + len] == buffer[cur + len]) {

					while (++len != len_limit)
						if (buffer[pby1 + len] != buffer[cur + len])
							break;

					if (max_len < len) {

						distances[offset++] = max_len = len;
						distances[offset++] = delta - 1;

						if (len == len_limit) {

							son[ptr1] = son[cyclic];
							son[ptr0] = son[cyclic + 1];

							break;
						}

					}

				}

				if (buffer[pby1 + len] < buffer[cur + len]) {

					son[ptr1] = cur_match;
					ptr1 = cyclic + 1;
					cur_match = son[ptr1];
					len1 = len;

				} else {

					son[ptr0] = cur_match;
					ptr0 = cyclic;
					cur_match = son[ptr0];
					len0 = len;

				}

			}

			move_pos();

			return offset;
		}

		void skip(u32 num) {

			const auto& crc{ tables().crc };

			do {

				u32 len_limit;

				if (pos + match_max_len <= stream_pos)
					len_limit = match_max_len;
				else {

					len_limit = stream_pos - pos;

					if (len_limit < MIN_MATCH_CHECK) {

						move_pos();

						continue;
					}

				}

				const u32 match_min_pos{ pos > cyclic_size ? pos - cyclic_size : 0 };
				const u32 cur{ buffer_offset + pos };

				u32 temp{ crc[buffer[cur]] ^ buffer[cur + 1] };

				hash[temp & (HASH2_SIZE - 1)] = pos;
				temp ^= (u32)buffer[cur + 2] << 8;
				hash[HASH3_OFFSET + (temp & (HASH3_SIZE - 1))] = pos;

				const u32 hash_value{ (temp ^ (crc[buffer[cur + 3]] << 5)) & hash_mask };

				u32 cur_match{ hash[FIX_HASH_SIZE + hash_value] };

				hash[FIX_HASH_SIZE + hash_value] = pos;

				u32 ptr0{ (cyclic_pos << 1) + 1 }, ptr1{ cyclic_pos << 1 };
				u32 len0{}, len1{};

				for (u32 count{ cut_value }; ; ) {

					if (cur_match <= match_min_pos || count-- == 0) {

						son[ptr0] = son[ptr1] = 0;

						break;
					}

					const u32 delta{ pos - cur_match };
					const u32 cyclic{ (delta <= cyclic_pos ? cyclic_pos - delta : cyclic_pos - delta + cyclic_size) << 1 };
					const u32 pby1{ buffer_offset + cur_match };

					u32 len{ std::min(len0, len1) };

					if (buffer[pby1 + len] == buffer[cur + len]) {

						while (++len != len_limit)
							if (buffer[pby1 + len] != buffer[cur + len])
								break;

						if (len == len_limit) {

							son[ptr1] = son[cyclic];
							son[ptr0] = son[cyclic + 1];

							break;
						}

					}

					if (buffer[pby1 + len] < buffer[cur + len]) {

						son[ptr1] = cur_match;
						ptr1 = cyclic + 1;
						cur_match = son[ptr1];
						len1 = len;

					} else {

						son[ptr0] = cur_match;
						ptr0 = cyclic;
						cur_match = son[ptr0];
						len0 = len;

					}

				}

				move_pos();

			} while (--num != 0);

		}
	};

	struct _optimal {

		_state state;
		bool prev1_is_char{}, prev2{};
		u32 pos_prev2{}, back_prev2{}, price{}, pos_prev{}, back_prev{};
		u32 backs[NUM_REP_DISTANCES]{};

		void make_as_char() { back_prev = 0xFFFFFFFF; prev1_is_char = false; }
		void make_as_short_rep() { back_prev = 0; prev1_is_char = false; }
		bool is_short_rep() const { return back_prev == 0; }
	};

	struct _encoder {

		static constexpr u32 POS_STATE_MASK{ (1u << POS_STATE_BITS) - 1 };
		static constexpr u32 DIST_TABLE_SIZE{ 21 * 2 }; // log2 of the dictionary size, times 2

		_state state;
		u8 previous_byte{};
		u32 rep_distances[NUM_REP_DISTANCES]{};

		std::vector<_optimal> optimum = std::vector<_optimal>(NUM_OPTS);
		_match_finder mf;
		_range_encoder rc;

		std::vector<_bit> is_match = std::vector<_bit>(NUM_STATES << NUM_POS_STATES_BITS_MAX);
		std::vector<_bit> is_rep0_long = std::vector<_bit>(NUM_STATES << NUM_POS_STATES_BITS_MAX);
		_bit is_rep[NUM_STATES]{}, is_rep_g0[NUM_STATES]{}, is_rep_g1[NUM_STATES]{}, is_rep_g2[NUM_STATES]{};
		std::vector<_bit_tree> pos_slot_encoder;
		std::vector<_bit> pos_encoders = std::vector<_bit>(NUM_FULL_DISTANCES - END_POS_MODEL_INDEX);
		_bit_tree pos_align_encoder{ NUM_ALIGN_BITS };
		_len_encoder len_encoder, rep_match_len_encoder;
		std::vector<_literal_coder> literal = std::vector<_literal_coder>(size_t(1) << (LIT_CONTEXT_BITS + LIT_POS_BITS));

		u32 match_distances[MATCH_MAX_LEN * 2 + 2]{};
		u32 longest_match_length{}, num_distance_pairs{}, additional_offset{};
		u32 optimum_end_index{}, optimum_current_index{};
		bool longest_match_was_found{};

		u32 pos_slot_prices[1u << (NUM_POS_SLOT_BITS + NUM_LEN_TO_POS_STATES_BITS)]{};
		u32 distances_prices[NUM_FULL_DISTANCES << NUM_LEN_TO_POS_STATES_BITS]{};
		u32 align_prices[ALIGN_TABLE_SIZE]{};
		u32 temp_prices[NUM_FULL_DISTANCES]{};
		u32 align_price_count{}, match_price_count{};

		u32 reps[NUM_REP_DISTANCES]{}, rep_lens[NUM_REP_DISTANCES]{};

		_encoder() {

			for (u32 i{}; i < NUM_LEN_TO_POS_STATES; ++i)
				pos_slot_encoder.emplace_back(NUM_POS_SLOT_BITS);

		}

		_literal_coder& literal_coder(u32 pos, u8 prev_byte) {
			return literal[((pos & ((1u << LIT_POS_BITS) - 1)) << LIT_CONTEXT_BITS) + (u32)(prev_byte >> (8 - LIT_CONTEXT_BITS))];
		}

		void init() {

			for (u32 i{}; i < NUM_STATES; ++i) {

				for (u32 j{}; j <= POS_STATE_MASK; ++j) {

					is_match[(i << NUM_POS_STATES_BITS_MAX) + j].init();
					is_rep0_long[(i << NUM_POS_STATES_BITS_MAX) + j].init();

				}

				is_rep[i].init();
				is_rep_g0[i].init();
				is_rep_g1[i].init();
				is_rep_g2[i].init();

			}

			for (auto& l : literal)
				l.init();

			for (auto& t : pos_slot_encoder)
				t.init();

			for (auto& b : pos_encoders)
				b.init();

			len_encoder.init(1u << POS_STATE_BITS);
			rep_match_len_encoder.init(1u << POS_STATE_BITS);
			pos_align_encoder.init();

		}

		void read_match_distances(u32& len_res, u32& num_pairs) {

			len_res = 0;
			num_pairs = mf.get_matches(match_distances);

			if (num_pairs > 0) {

				len_res = match_distances[num_pairs - 2];

				if (len_res == NUM_FAST_BYTES)
					len_res += mf.match_len((i32)len_res - 1, match_distances[num_pairs - 1], MATCH_MAX_LEN - len_res);

			}

			++additional_offset;

		}

		void move_pos(u32 num) {

			if (num > 0) {

				mf.skip(num);
				additional_offset += num;

			}

		}

		u32 rep_len1_price(_state s, u32 pos_state) const {
			return is_rep_g0[s.index].price0() + is_rep0_long[(s.index << NUM_POS_STATES_BITS_MAX) + pos_state].price0();
		}

		u32 pure_rep_price(u32 rep_index, _state s, u32 pos_state) const {

			u32 p;

			if (rep_index == 0) {

				p = is_rep_g0[s.index].price0();
				p += is_rep0_long[(s.index << NUM_POS_STATES_BITS_MAX) + pos_state].price1();

			} else {

				p = is_rep_g0[s.index].price1();

				if (rep_index == 1)
					p += is_rep_g1[s.index].price0();
				else {

					p += is_rep_g1[s.index].price1();
					p += is_rep_g2[s.index].price(rep_index - 2);

				}

			}

			return p;
		}

		u32 rep_price(u32 rep_index, u32 len, _state s, u32 pos_state) const {
			return rep_match_len_encoder.price(len - MATCH_MIN_LEN, pos_state) + pure_rep_price(rep_index, s, pos_state);
		}

		u32 pos_len_price(u32 pos, u32 len, u32 pos_state) const {

			const u32 lps{ len_to_pos_state(len) };

			const u32 p{ pos < NUM_FULL_DISTANCES ? distances_prices[lps * NUM_FULL_DISTANCES + pos] :
				pos_slot_prices[(lps << NUM_POS_SLOT_BITS) + pos_slot2(pos)] + align_prices[pos & ALIGN_MASK] };

			return p + len_encoder.price(len - MATCH_MIN_LEN, pos_state);
		}

		u32 backward(u32& back_res, u32 cur) {

			optimum_end_index = cur;

			u32 pos_mem{ optimum[cur].pos_prev }, back_mem{ optimum[cur].back_prev };

			do {

				if (optimum[cur].prev1_is_char) {

					optimum[pos_mem].make_as_char();
					optimum[pos_mem].pos_prev = pos_mem - 1;

					if (optimum[cur].prev2) {

						optimum[pos_mem - 1].prev1_is_char = false;
						optimum[pos_mem - 1].pos_prev = optimum[cur].pos_prev2;
						optimum[pos_mem - 1].back_prev = optimum[cur].back_prev2;

					}

				}

				const u32 pos_prev{ pos_mem }, back_cur{ back_mem };

				back_mem = optimum[pos_prev].back_prev;
				pos_mem = optimum[pos_prev].pos_prev;
				optimum[pos_prev].back_prev = back_cur;
				optimum[pos_prev].pos_prev = cur;
				cur = pos_prev;

			} while (cur > 0);

			back_res = optimum[0].back_prev;
			optimum_current_index = optimum[0].pos_prev;

			return optimum_current_index;
		}

		u32 get_optimum(u32 position, u32& back_res) {

			if (optimum_end_index != optimum_current_index) {

				const u32 len_res{ optimum[optimum_current_index].pos_prev - optimum_current_index };

				back_res = optimum[optimum_current_index].back_prev;
				optimum_current_index = optimum[optimum_current_index].pos_prev;

				return len_res;
			}

			optimum_current_index = optimum_end_index = 0;

			u32 len_main, num_pairs;

			if (!longest_match_was_found)
				read_match_distances(len_main, num_pairs);
			else {

				len_main = longest_match_length;
				num_pairs = num_distance_pairs;
				longest_match_was_found = false;

			}

			u32 num_available{ mf.available() + 1 };

			if (num_available < 2) {

				back_res = 0xFFFFFFFF;

				return 1;
			}

			if (num_available > MATCH_MAX_LEN)
				num_available = MATCH_MAX_LEN;

			u32 rep_max_index{};

			for (u32 i{}; i < NUM_REP_DISTANCES; ++i) {

				reps[i] = rep_distances[i];
				rep_lens[i] = mf.match_len(-1, reps[i], MATCH_MAX_LEN);

				if (rep_lens[i] > rep_lens[rep_max_index])
					rep_max_index = i;

			}

			if (rep_lens[rep_max_index] >= NUM_FAST_BYTES) {

				back_res = rep_max_index;

				const u32 len_res{ rep_lens[rep_max_index] };

				move_pos(len_res - 1);

				return len_res;
			}

			if (len_main >= NUM_FAST_BYTES) {

				back_res = match_distances[num_pairs - 1] + NUM_REP_DISTANCES;
				move_pos(len_main - 1);

				return len_main;
			}

			u8 current_byte{ mf.index_byte(-1) };
			u8 match_byte{ mf.index_byte((i32)(0u - rep_distances[0] - 1 - 1)) };

			if (len_main < 2 && current_byte != match_byte && rep_lens[rep_max_index] < 2) {

				back_res = 0xFFFFFFFF;

				return 1;
			}

			optimum[0].state = state;

			u32 pos_state{ position & POS_STATE_MASK };

			optimum[1].price = is_match[(state.index << NUM_POS_STATES_BITS_MAX) + pos_state].price0() +
				literal_coder(position, previous_byte).price(!state.is_char(), match_byte, current_byte);
			optimum[1].make_as_char();

			u32 match_price{ is_match[(state.index << NUM_POS_STATES_BITS_MAX) + pos_state].price1() };
			u32 rep_match_price{ match_price + is_rep[state.index].price1() };

			if (match_byte == current_byte) {

				const u32 short_rep_price{ rep_match_price + rep_len1_price(state, pos_state) };

				if (short_rep_price < optimum[1].price) {

					optimum[1].price = short_rep_price;
					optimum[1].make_as_short_rep();

				}

			}

			u32 len_end{ len_main >= rep_lens[rep_max_index] ? len_main : rep_lens[rep_max_index] };

			if (len_end < 2) {

				back_res = optimum[1].back_prev;

				return 1;
			}

			optimum[1].pos_prev = 0;

			for (u32 i{}; i < NUM_REP_DISTANCES; ++i)
				optimum[0].backs[i] = reps[i];

			u32 len{ len_end };

			do
				optimum[len--].price = INFINITY_PRICE;
			while (len >= 2);

			for (u32 i{}; i < NUM_REP_DISTANCES; ++i) {

				u32 rep_len{ rep_lens[i] };

				if (rep_len < 2)
					continue;

				const u32 price{ rep_match_price + pure_rep_price(i, state, pos_state) };

				do {

					const u32 cur_and_len_price{ price + rep_match_len_encoder.price(rep_len - 2, pos_state) };
					auto& o{ optimum[rep_len] };

					if (cur_and_len_price < o.price) {

						o.price = cur_and_len_price;
						o.pos_prev = 0;
						o.back_prev = i;
						o.prev1_is_char = false;

					}

				} while (--rep_len >= 2);

			}

			u32 normal_match_price{ match_price + is_rep[state.index].price0() };

			len = rep_lens[0] >= 2 ? rep_lens[0] + 1 : 2;

			if (len <= len_main) {

				u32 offs{};

				while (len > match_distances[offs])
					offs += 2;

				for (; ; ++len) {

					const u32 distance{ match_distances[offs + 1] };
					const u32 cur_and_len_price{ normal_match_price + pos_len_price(distance, len, pos_state) };
					auto& o{ optimum[len] };

					if (cur_and_len_price < o.price) {

						o.price = cur_and_len_price;
						o.pos_prev = 0;
						o.back_prev = distance + NUM_REP_DISTANCES;
						o.prev1_is_char = false;

					}

					if (len == match_distances[offs]) {

						offs += 2;

						if (offs == num_pairs)
							break;
					}

				}

			}

			for (u32 cur{};;) {

				++cur;

				if (cur == len_end)
					return backward(back_res, cur);

				u32 new_len;

				read_match_distances(new_len, num_pairs);

				if (new_len >= NUM_FAST_BYTES) {

					num_distance_pairs = num_pairs;
					longest_match_length = new_len;
					longest_match_was_found = true;

					return backward(back_res, cur);
				}

				++position;

				u32 pos_prev{ optimum[cur].pos_prev };
				_state s;

				if (optimum[cur].prev1_is_char) {

					--pos_prev;

					if (optimum[cur].prev2) {

						s = optimum[optimum[cur].pos_prev2].state;

						if (optimum[cur].back_prev2 < NUM_REP_DISTANCES)
							s.update_rep();
						else
							s.update_match();

					} else
						s = optimum[pos_prev].state;

					s.update_char();

				} else
					s = optimum[pos_prev].state;

				if (pos_prev == cur - 1) {

					if (optimum[cur].is_short_rep())
						s.update_short_rep();
					else
						s.update_char();

				} else {

					u32 pos;

					if (optimum[cur].prev1_is_char && optimum[cur].prev2) {

						pos_prev = optimum[cur].pos_prev2;
						pos = optimum[cur].back_prev2;
						s.update_rep();

					} else {

						pos = optimum[cur].back_prev;

						if (pos < NUM_REP_DISTANCES)
							s.update_rep();
						else
							s.update_match();

					}

					const auto& opt{ optimum[pos_prev] };

					if (pos < NUM_REP_DISTANCES) {

						// the used rep moves to the front
						reps[0] = opt.backs[pos];

						for (u32 i{ 1 }, k{}; i < NUM_REP_DISTANCES; ++k)
							if (k != pos)
								reps[i++] = opt.backs[k];

					} else {

						reps[0] = pos - NUM_REP_DISTANCES;
						reps[1] = opt.backs[0];
						reps[2] = opt.backs[1];
						reps[3] = opt.backs[2];

					}

				}

				optimum[cur].state = s;

				for (u32 i{}; i < NUM_REP_DISTANCES; ++i)
					optimum[cur].backs[i] = reps[i];

				const u32 cur_price{ optimum[cur].price };

				current_byte = mf.index_byte(-1);
				match_byte = mf.index_byte((i32)(0u - reps[0] - 1 - 1));
				pos_state = position & POS_STATE_MASK;

				const u32 cur_and1_price{ cur_price + is_match[(s.index << NUM_POS_STATES_BITS_MAX) + pos_state].price0() +
					literal_coder(position, mf.index_byte(-2)).price(!s.is_char(), match_byte, current_byte) };

				auto& next{ optimum[cur + 1] };
				bool next_is_char{};

				if (cur_and1_price < next.price) {

					next.price = cur_and1_price;
					next.pos_prev = cur;
					next.make_as_char();
					next_is_char = true;

				}

				match_price = cur_price + is_match[(s.index << NUM_POS_STATES_BITS_MAX) + pos_state].price1();
				rep_match_price = match_price + is_rep[s.index].price1();

				if (match_byte == current_byte && !(next.pos_prev < cur && next.back_prev == 0)) {

					const u32 short_rep_price{ rep_match_price + rep_len1_price(s, pos_state) };

					if (short_rep_price <= next.price) {

						next.price = short_rep_price;
						next.pos_prev = cur;
						next.make_as_short_rep();
						next_is_char = true;

					}

				}

				const u32 num_available_full{ std::min(NUM_OPTS - 1 - cur, mf.available() + 1) };

				num_available = num_available_full;

				if (num_available < 2)
					continue;

				if (num_available > NUM_FAST_BYTES)
					num_available = NUM_FAST_BYTES;

				if (!next_is_char && match_byte != current_byte) {

					// a literal, then rep0
					const u32 t{ std::min(num_available_full - 1, NUM_FAST_BYTES) };
					const u32 len_test2{ mf.match_len(0, reps[0], t) };

					if (len_test2 >= 2) {

						_state s2{ s };

						s2.update_char();

						const u32 pos_state_next{ (position + 1) & POS_STATE_MASK };
						const u32 next_rep_match_price{ cur_and1_price +
							is_match[(s2.index << NUM_POS_STATES_BITS_MAX) + pos_state_next].price1() + is_rep[s2.index].price1() };

						const u32 offset{ cur + 1 + len_test2 };

						while (len_end < offset)
							optimum[++len_end].price = INFINITY_PRICE;

						const u32 cur_and_len_price{ next_rep_match_price + rep_price(0, len_test2, s2, pos_state_next) };
						auto& o{ optimum[offset] };

						if (cur_and_len_price < o.price) {

							o.price = cur_and_len_price;
							o.pos_prev = cur + 1;
							o.back_prev = 0;
							o.prev1_is_char = true;
							o.prev2 = false;

						}

					}

				}

				u32 start_len{ 2 }; // speed optimization

				for (u32 rep_index{}; rep_index < NUM_REP_DISTANCES; ++rep_index) {

					u32 len_test{ mf.match_len(-1, reps[rep_index], num_available) };

					if (len_test < 2)
						continue;

					const u32 len_test_temp{ len_test };

					do {

						while (len_end < cur + len_test)
							optimum[++len_end].price = INFINITY_PRICE;

						const u32 cur_and_len_price{ rep_match_price + rep_price(rep_index, len_test, s, pos_state) };
						auto& o{ optimum[cur + len_test] };

						if (cur_and_len_price < o.price) {

							o.price = cur_and_len_price;
							o.pos_prev = cur;
							o.back_prev = rep_index;
							o.prev1_is_char = false;

						}

					} while (--len_test >= 2);

					len_test = len_test_temp;

					if (rep_index == 0)
						start_len = len_test + 1;

					if (len_test < num_available_full) {

						const u32 t{ std::min(num_available_full - 1 - len_test, NUM_FAST_BYTES) };
						const u32 len_test2{ mf.match_len((i32)len_test, reps[rep_index], t) };

						if (len_test2 >= 2) {

							_state s2{ s };

							s2.update_rep();

							u32 pos_state_next{ (position + len_test) & POS_STATE_MASK };

							const u32 cur_and_len_char_price{ rep_match_price + rep_price(rep_index, len_test, s, pos_state) +
								is_match[(s2.index << NUM_POS_STATES_BITS_MAX) + pos_state_next].price0() +
								literal_coder(position + len_test, mf.index_byte((i32)len_test - 1 - 1)).price(true,
									mf.index_byte((i32)len_test - 1 - (i32)(reps[rep_index] + 1)), mf.index_byte((i32)len_test - 1)) };

							s2.update_char();
							pos_state_next = (position + len_test + 1) & POS_STATE_MASK;

							const u32 next_match_price{ cur_and_len_char_price + is_match[(s2.index << NUM_POS_STATES_BITS_MAX) + pos_state_next].price1() };
							const u32 next_rep_match_price{ next_match_price + is_rep[s2.index].price1() };

							const u32 offset{ len_test + 1 + len_test2 };

							while (len_end < cur + offset)
								optimum[++len_end].price = INFINITY_PRICE;

							const u32 cur_and_len_price{ next_rep_match_price + rep_price(0, len_test2, s2, pos_state_next) };
							auto& o{ optimum[cur + offset] };

							if (cur_and_len_price < o.price) {

								o.price = cur_and_len_price;
								o.pos_prev = cur + len_test + 1;
								o.back_prev = 0;
								o.prev1_is_char = true;
								o.prev2 = true;
								o.pos_prev2 = cur;
								o.back_prev2 = rep_index;

							}

						}

					}

				}

				if (new_len > num_available) {

					new_len = num_available;

					for (num_pairs = 0; new_len > match_distances[num_pairs]; num_pairs += 2);

					match_distances[num_pairs] = new_len;
					num_pairs += 2;

				}

				if (new_len >= start_len) {

					normal_match_price = match_price + is_rep[s.index].price0();

					while (len_end < cur + new_len)
						optimum[++len_end].price = INFINITY_PRICE;

					u32 offs{};

					while (start_len > match_distances[offs])
						offs += 2;

					for (u32 len_test{ start_len }; ; ++len_test) {

						const u32 cur_back{ match_distances[offs + 1] };
						u32 cur_and_len_price{ normal_match_price + pos_len_price(cur_back, len_test, pos_state) };

						{
							auto& o{ optimum[cur + len_test] };

							if (cur_and_len_price < o.price) {

								o.price = cur_and_len_price;
								o.pos_prev = cur;
								o.back_prev = cur_back + NUM_REP_DISTANCES;
								o.prev1_is_char = false;

							}
						}

						if (len_test == match_distances[offs]) {

							if (len_test < num_available_full) {

								const u32 t{ std::min(num_available_full - 1 - len_test, NUM_FAST_BYTES) };
								const u32 len_test2{ mf.match_len((i32)len_test, cur_back, t) };

								if (len_test2 >= 2) {

									_state s2{ s };

									s2.update_match();

									u32 pos_state_next{ (position + len_test) & POS_STATE_MASK };

									const u32 cur_and_len_char_price{ cur_and_len_price +
										is_match[(s2.index << NUM_POS_STATES_BITS_MAX) + pos_state_next].price0() +
										literal_coder(position + len_test, mf.index_byte((i32)len_test - 1 - 1)).price(true,
											mf.index_byte((i32)len_test - (i32)(cur_back + 1) - 1), mf.index_byte((i32)len_test - 1)) };

									s2.update_char();
									pos_state_next = (position + len_test + 1) & POS_STATE_MASK;

									const u32 next_match_price{ cur_and_len_char_price + is_match[(s2.index << NUM_POS_STATES_BITS_MAX) + pos_state_next].price1() };
									const u32 next_rep_match_price{ next_match_price + is_rep[s2.index].price1() };

									const u32 offset{ len_test + 1 + len_test2 };

									while (len_end < cur + offset)
										optimum[++len_end].price = INFINITY_PRICE;

									cur_and_len_price = next_rep_match_price + rep_price(0, len_test2, s2, pos_state_next);

									auto& o{ optimum[cur + offset] };

									if (cur_and_len_price < o.price) {

										o.price = cur_and_len_price;
										o.pos_prev = cur + len_test + 1;
										o.back_prev = 0;
										o.prev1_is_char = true;
										o.prev2 = true;
										o.pos_prev2 = cur;
										o.back_prev2 = cur_back + NUM_REP_DISTANCES;

									}

								}

							}

							offs += 2;

							if (offs == num_pairs)
								break;
						}

					}

				}

			}

		}

		void fill_distances_prices() {

			for (u32 i{ START_POS_MODEL_INDEX }; i < NUM_FULL_DISTANCES; ++i) {

				const u32 slot{ pos_slot(i) };
				const int footer_bits{ (int)((slot >> 1) - 1) };
				const u32 base{ (2 | (slot & 1)) << footer_bits };

				temp_prices[i] = _bit_tree::reverse_price(pos_encoders.data(), base - slot - 1, footer_bits, i - base);

			}

			for (u32 lps{}; lps < NUM_LEN_TO_POS_STATES; ++lps) {

				const auto& encoder{ pos_slot_encoder[lps] };
				const u32 st{ lps << NUM_POS_SLOT_BITS };

				for (u32 slot{}; slot < DIST_TABLE_SIZE; ++slot)
					pos_slot_prices[st + slot] = encoder.price(slot);

				for (u32 slot{ END_POS_MODEL_INDEX }; slot < DIST_TABLE_SIZE; ++slot)
					pos_slot_prices[st + slot] += (((slot >> 1) - 1) - NUM_ALIGN_BITS) << 6;

				const u32 st2{ lps * NUM_FULL_DISTANCES };

				u32 i{};

				for (; i < START_POS_MODEL_INDEX; ++i)
					distances_prices[st2 + i] = pos_slot_prices[st + i];

				for (; i < NUM_FULL_DISTANCES; ++i)
					distances_prices[st2 + i] = pos_slot_prices[st + pos_slot(i)] + temp_prices[i];

			}

			match_price_count = 0;

		}

		void fill_align_prices() {

			for (u32 i{}; i < ALIGN_TABLE_SIZE; ++i)
				align_prices[i] = pos_align_encoder.reverse_price(i);

			align_price_count = 0;

		}

		void encode(std::span<const u8> data, std::vector<u8>& out) {

			mf.create(DICTIONARY_SIZE, NUM_OPTS, NUM_FAST_BYTES, MATCH_MAX_LEN + 1);
			rc.out = &out;
			init();
			fill_distances_prices();
			fill_align_prices();

			len_encoder.table_size = NUM_FAST_BYTES + 1 - MATCH_MIN_LEN;
			len_encoder.update_tables(1u << POS_STATE_BITS);
			rep_match_len_encoder.table_size = NUM_FAST_BYTES + 1 - MATCH_MIN_LEN;
			rep_match_len_encoder.update_tables(1u << POS_STATE_BITS);

			mf.input = data;
			mf.init();

			if (mf.available() == 0) {

				rc.flush();

				return;
			}

			// the first byte is always a literal
			{

				u32 len, num_pairs;

				read_match_distances(len, num_pairs);

				is_match[state.index << NUM_POS_STATES_BITS_MAX].encode(rc, 0);
				state.update_char();

				const u8 cur_byte{ mf.index_byte((i32)(0u - additional_offset)) };

				literal_coder(0, previous_byte).encode(rc, cur_byte);
				previous_byte = cur_byte;
				--additional_offset;

			}

			u64 now_pos{ 1 };

			// the match finder runs ahead of what is coded: stop only once everything it read is coded
			for (bool more{ mf.available() != 0 }; more; ) {

				u32 pos;
				const u32 len{ get_optimum((u32)now_pos, pos) };
				const u32 pos_state{ (u32)now_pos & POS_STATE_MASK };
				const u32 complex_state{ (state.index << NUM_POS_STATES_BITS_MAX) + pos_state };

				if (len == 1 && pos == 0xFFFFFFFF) {

					is_match[complex_state].encode(rc, 0);

					const u8 cur_byte{ mf.index_byte((i32)(0u - additional_offset)) };
					auto& sub{ literal_coder((u32)now_pos, previous_byte) };

					if (!state.is_char())
						sub.encode_matched(rc, mf.index_byte((i32)(0u - rep_distances[0] - 1 - additional_offset)), cur_byte);
					else
						sub.encode(rc, cur_byte);

					previous_byte = cur_byte;
					state.update_char();

				} else {

					is_match[complex_state].encode(rc, 1);

					if (pos < NUM_REP_DISTANCES) {

						is_rep[state.index].encode(rc, 1);

						if (pos == 0) {

							is_rep_g0[state.index].encode(rc, 0);
							is_rep0_long[complex_state].encode(rc, len == 1 ? 0 : 1);

						} else {

							is_rep_g0[state.index].encode(rc, 1);

							if (pos == 1)
								is_rep_g1[state.index].encode(rc, 0);
							else {

								is_rep_g1[state.index].encode(rc, 1);
								is_rep_g2[state.index].encode(rc, pos - 2);

							}

						}

						if (len == 1)
							state.update_short_rep();
						else {

							rep_match_len_encoder.encode(rc, len - MATCH_MIN_LEN, pos_state);
							state.update_rep();

						}

						const u32 distance{ rep_distances[pos] };

						if (pos != 0) {

							for (u32 i{ pos }; i >= 1; --i)
								rep_distances[i] = rep_distances[i - 1];

							rep_distances[0] = distance;

						}

					} else {

						is_rep[state.index].encode(rc, 0);
						state.update_match();
						len_encoder.encode(rc, len - MATCH_MIN_LEN, pos_state);
						pos -= NUM_REP_DISTANCES;

						const u32 slot{ pos_slot(pos) };

						pos_slot_encoder[len_to_pos_state(len)].encode(rc, slot);

						if (slot >= START_POS_MODEL_INDEX) {

							const int footer_bits{ (int)((slot >> 1) - 1) };
							const u32 base{ (2 | (slot & 1)) << footer_bits };
							const u32 pos_reduced{ pos - base };

							if (slot < END_POS_MODEL_INDEX)
								_bit_tree::reverse_encode(pos_encoders.data(), base - slot - 1, rc, footer_bits, pos_reduced);
							else {

								rc.encode_direct_bits(pos_reduced >> NUM_ALIGN_BITS, footer_bits - NUM_ALIGN_BITS);
								pos_align_encoder.reverse_encode(rc, pos_reduced & ALIGN_MASK);
								++align_price_count;

							}

						}

						for (u32 i{ NUM_REP_DISTANCES - 1 }; i >= 1; --i)
							rep_distances[i] = rep_distances[i - 1];

						rep_distances[0] = pos;
						++match_price_count;

					}

					previous_byte = mf.index_byte((i32)(len - 1 - additional_offset));

				}

				additional_offset -= len;
				now_pos += len;

				if (additional_offset == 0) {

					if (match_price_count >= (1u << 7))
						fill_distances_prices();

					if (align_price_count >= ALIGN_TABLE_SIZE)
						fill_align_prices();

					more = mf.available() != 0;

				}

			}

			rc.flush();

		}
	};

	// as osu! stores replay data: the 5 LZMA property bytes, the input size as a u64, then the stream
	inline std::vector<u8> compress(std::span<const u8> data) {

		std::vector<u8> out{ (u8)((POS_STATE_BITS * 5 + LIT_POS_BITS) * 9 + LIT_CONTEXT_BITS) };

		for (int i{}; i < 4; ++i)
			out.push_back((u8)(DICTIONARY_SIZE >> (8 * i)));

		for (int i{}; i < 8; ++i)
			out.push_back((u8)((u64)data.size() >> (8 * i)));

		out.reserve(out.size() + data.size() / 2);

		auto e{ std::make_unique<_encoder>() };

		e->encode(data, out);

		return out;
	}

}

}
