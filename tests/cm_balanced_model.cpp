// BALANCED uses the first contextual counter at confidence >= 8, otherwise its fallback.
#include <tuple>
#include "orz_shared.h"
#include "cm.h"

using namespace orz;

void require(bool ok) {
	if (!ok)
		throw std::runtime_error("balanced prediction differs from counter oracle");
}

struct oracle_counter {
	u16 p, n;

	void update(int bit) {
		const i64 rate{ 131072 / (2 * (i64)n + 3) };
		p = (u16)(p + (((bit ? 65535 : 0) - (i64)p) * rate >> 16));
		if (n < 255)
			++n;
	}
};

template<bool Training>
struct probe : std::conditional_t<Training, cm::balanced_counter_trainer, cm::balanced_bit_encoder> {
	using observation = std::tuple<size_t, int, u16, u16>;
	std::vector<observation> seen;
	int calls{}, probability{}, next_bit{};

	int code(int, int p) {
		++calls;
		probability = p;
		return next_bit;
	}

	void observe(const cm::line_table& table, size_t at, int bit) {
		seen.emplace_back(at, bit, table.t[at].p, table.t[at].n);
	}
};

template<int N, typename Model, bool Training>
void check(int bits) {

	Model m;
	if constexpr (std::is_same_v<Model, cm::model>) m.init(N, bits, 8, 3, true);
	else m.init(bits, 8, 3, true);

	constexpr oracle_counter starts[]{ { 0, 0 }, { 65535, 7 }, { 31, 8 }, { 65504, 255 }, { 32768, 1 }, { 12345, 254 } };
	std::vector<oracle_counter> expected;
	for (size_t i{}; i < m.table.t.size(); ++i) {
		const auto k{ starts[i % std::size(starts)] };
		m.table.t[i] = { k.p, k.n };
		expected.push_back(k);
	}
	const auto weights{ m.weights };
	u64 ctx[N]{};
	probe<Training> c;

	for (u32 step{}; step < 512; ++step) {

		for (int i{}; i < N; ++i)
			ctx[i] = (u64)step * 7919 + (u64)i * 104729;
		const bool single{ step % 11 == 0 };
		const u64 salt{ single ? 0u : step % 2 };
		const u32 slot{ single ? 0u : step % 8 };
		const size_t at[]{ (cm::hash(salt * 8, ctx[0]) & m.table.mask) * 8 + slot,
			(cm::hash(salt * 8 + 7, 0) & m.table.mask) * 8 + slot };
		const auto prediction{ expected[expected[at[0]].n >= 8 ? at[0] : at[1]] };
		const int input{ (int)((step * 37 + step / 7) % 17 < 9) };
		const int actual{ Training ? input : input ^ (int)(step % 3 == 0) };
		c.next_bit = actual;
		c.calls = 0;
		c.seen.clear();
		int got;

		if (single)
			got = m.code(c, input, ctx, step % 3);
		else {
			if constexpr (std::is_same_v<Model, cm::model> && N == 4)
				m.template bind_n<N, probe<Training>>(ctx, salt);
			else
				m.template bind<probe<Training>>(ctx, salt);
			got = m.code(c, input, slot, step % 3);
		}
		require(got == actual && c.calls == (Training ? 0 : 1));
		if constexpr (!Training)
			require(c.probability == std::clamp((int)prediction.p, 32, 65504));
		require(c.seen.size() == 2);
		for (int i{}; i < 2; ++i) {
			const auto before{ expected[at[i]] };
			require(c.seen[i] == typename probe<Training>::observation{ at[i], actual, before.p, before.n });
			expected[at[i]].update(actual); // aliases intentionally update twice, in order
		}
		for (size_t i{}; i < expected.size(); ++i)
			require(m.table.t[i].p == expected[i].p && m.table.t[i].n == expected[i].n);
		require(m.weights == weights);

	}

}

template<int N>
void check_count() {
	for (int bits : { 0, 4 }) {
		check<N, cm::fixed_model<N>, false>(bits);
		check<N, cm::fixed_model<N>, true>(bits);
		check<N, cm::model, false>(bits);
		check<N, cm::model, true>(bits);
	}
}

int main() {
	check_count<1>(); check_count<2>(); check_count<3>(); check_count<4>();
	check_count<5>(); check_count<6>(); check_count<7>();
	puts("balanced confidence, probabilities, trainer and ordered alias updates passed");
}
