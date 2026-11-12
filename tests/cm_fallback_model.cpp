// cl /std:c++20 /O2 /EHsc /I. /ILib tests/cm_fallback_model.cpp /Fe:cm_fallback_model.exe
// The fallback profile must code the selected slot directly and update it exactly once.

#include <tuple>
#include "orz_shared.h"
#include "cm.h"

using namespace orz;

void require(bool ok) {
	if (!ok)
		throw std::runtime_error("fallback model differs from counter oracle");
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
struct probe : std::conditional_t<Training, cm::fallback_counter_trainer, cm::fastest_bit_encoder> {

	using observation = std::tuple<size_t, int, u16, u16, u64>;
	std::vector<observation> seen;
	int calls{}, input{}, probability{}, next_bit{};

	int code(int bit, int p) {
		++calls;
		input = bit;
		probability = p;
		return next_bit;
	}

	void observe(const cm::line_table& table, size_t at, int bit) {
		seen.emplace_back(at, bit, table.t[at].p, table.t[at].n,
			table.keys.empty() ? 0 : table.keys[at / table.width]);
	}
};

template<int N, typename Model>
void init(Model& m, u32 width, bool record_keys, bool fallback_only) {
	if constexpr (std::is_same_v<Model, cm::model>)
		m.init(N, 8, width, 3, record_keys, fallback_only);
	else
		m.init(8, width, 3, record_keys, fallback_only);
}

template<int N, typename Model, bool Training>
void check(u32 width, bool seeded, bool record_keys) {

	Model m;
	init<N>(m, width, record_keys, true);
	require(m.inputs == N && m.mixed() == N + 1);
	require(m.table.width == width && m.table.t.size() == 2 * width);
	require(m.weights.empty());
	require(m.table.keys.size() == (record_keys ? 2u : 0u));

	// Include probability clamp edges and confidence saturation, in both salts and all decision slots.
	constexpr oracle_counter starts[]{ { 0, 0 }, { 65535, 0 }, { 31, 7 }, { 65504, 255 }, { 32768, 1 }, { 12345, 254 } };
	std::vector<oracle_counter> expected;
	for (size_t i{}; i < m.table.t.size(); ++i) {
		const oracle_counter value{ seeded ? starts[i % std::size(starts)] : oracle_counter{ 32768, 0 } };
		m.table.t[i] = { value.p, value.n };
		expected.push_back(value);
	}

	probe<Training> c;
	u64 ctx[N]{};
	for (u32 step{}; step < 512; ++step) {

		for (int i{}; i < N; ++i)
			ctx[i] = (u64)step * 7919 + (u64)i * 104729;

		const bool single{ step % 11 == 0 };
		const u64 salt{ single ? 0u : (step / width) % 2 };
		const u32 slot{ single ? 0u : step % width };
		const size_t at{ (size_t)salt * width + slot };
		const u64 key{ cm::hash(salt * 8 + 7, 0) };
		const int wanted{ (int)((step * 37 + step / 7) % 17 < 9) };
		const int actual{ Training ? wanted : wanted ^ (int)(step % 3 == 0) };
		const auto before{ expected[at] };
		c.next_bit = actual;
		c.calls = 0;
		c.seen.clear();
		int got;

		if (single)
			got = m.code(c, wanted, ctx, step % 3);
		else {
			if constexpr (std::is_same_v<Model, cm::model> && N == 4)
				m.template bind_n<N, probe<Training>>(ctx, salt);
			else
				m.template bind<probe<Training>>(ctx, salt);
			got = m.code(c, wanted, slot, step % 3);
		}

		require(got == actual && c.calls == (Training ? 0 : 1));
		if constexpr (!Training)
			require(c.input == wanted && c.probability == std::clamp((int)before.p, 32, 65504));

		const typename probe<Training>::observation observation{ at, actual, before.p, before.n, record_keys ? key : 0 };
		require(c.seen.size() == 1 && c.seen[0] == observation);
		expected[at].update(actual);
		for (size_t i{}; i < expected.size(); ++i)
			require(m.table.t[i].p == expected[i].p && m.table.t[i].n == expected[i].n);
		require(m.weights.empty());

	}

	if (record_keys) {
		require(m.table.keys[0] == cm::hash(7, 0));
		require(m.table.keys[1] == cm::hash(15, 0));
	}

	// Reusing a model for SIZE restores its full shape and initial mixer state.
	init<N>(m, width, record_keys, false);
	require(m.table.t.size() == size_t(width) * 256 && m.weights.size() == 3 * (N + 1));
	for (const auto& value : m.table.t)
		require(value.p == 32768 && value.n == 0);
	for (size_t i{}; i < m.weights.size(); ++i)
		require(m.weights[i] == cm::start_weight(i % (N + 1), N + 1));

}

template<int N>
void check_inputs() {
	for (u32 width : { 1u, 16u, 32u, 64u })
		for (bool seeded : { false, true })
			for (bool recording : { false, true }) {
				check<N, cm::model, false>(width, seeded, recording);
				check<N, cm::model, true>(width, seeded, recording);
				check<N, cm::fixed_model<N>, false>(width, seeded, recording);
				check<N, cm::fixed_model<N>, true>(width, seeded, recording);
			}
}

void recording_transition() {
	cm::fixed_model<6> m;
	m.init(8, 4, 3, true, true);
	const u64 ctx[6]{ 11, 22, 33, 44, 55, 66 };
	m.bind<cm::fallback_counter_trainer>(ctx, 1);
	require(m.table.keys.size() == 2 && m.table.keys[1] == cm::hash(15, 0));
	m.init(8, 4, 3, false, false);
	require(m.table.keys.empty());
	m.bind(ctx, 1);
	cm::bit_encoder encoder;
	require(m.code(encoder, 1, 3u, 0) == 1);
	m.init(8, 4, 3, false, true);
	require(m.table.keys.empty() && m.table.t.size() == 8);
}

int main() {
	recording_transition();
	check_inputs<1>();
	check_inputs<2>();
	check_inputs<3>();
	check_inputs<4>();
	check_inputs<5>();
	check_inputs<6>();
	check_inputs<7>();
}
