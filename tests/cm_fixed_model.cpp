// clang++ -std=c++20 -O2 -I. -ILib tests/cm_fixed_model.cpp -o /tmp/cm_fixed_model && /tmp/cm_fixed_model
// fixed input counts must preserve the adaptive state, coded bytes and prior trainer's observations.

#include "orz_shared.h"
#include "cm.h"
#include <tuple>

using namespace orz;

void require(bool ok) {
	if (!ok)
		throw std::runtime_error("fixed model differs from dynamic model");
}

void seed(cm::model& m) {

	for (size_t i{}; i < m.table.t.size(); ++i)
		m.table.t[i] = { (u16)(1024 + i * 487 % 63488), (u16)(i % 256) };

	for (size_t i{}; i < m.weights.size(); ++i)
		m.weights[i] += (int)(i % 7) * 1024;

}

void equal(const cm::model& a, const cm::model& b) {

	require(a.table.t.size() == b.table.t.size());

	for (size_t i{}; i < a.table.t.size(); ++i)
		require(a.table.t[i].p == b.table.t[i].p && a.table.t[i].n == b.table.t[i].n);

	require(a.table.keys == b.table.keys && a.weights == b.weights);

}

struct trainer : cm::bit_trainer {

	std::vector<std::tuple<size_t, u64, u16, u16, int>> seen;

	void observe(const cm::line_table& table, size_t at, int bit) {
		seen.emplace_back(at, table.keys[at / table.width], table.t[at].p, table.t[at].n, bit);
	}

};

template<typename Model, typename Coder>
void run(Model& m, Coder& coder) {

	u64 rng{ 127 };

	for (size_t i{}; i < 4096; ++i) {

		u64 ctx[cm::MAX_INPUTS];

		for (auto& value : ctx) {

			rng = rng * 6364136223846793005ull + 1;
			value = (rng >> 32) % 64;

		}

		const int bit{ (int)(rng >> 63) };
		int got;

		// force the first context onto the fallback's line, including with nonzero salts. Binding must stamp the
		// fallback's key last, and training must observe both updates to this shared counter in order.
		if (i % 8 < 2) {

			const u64 salt{ i % 2 ? i % 3 : 0 };
			const u64 fallback{ cm::hash(salt * cm::MAX_INPUTS + cm::FALLBACK_INPUT, 0) };

			while ((cm::hash(salt * cm::MAX_INPUTS, ctx[0]) & m.table.mask) != (fallback & m.table.mask))
				++ctx[0];

		}

		if (i % 2) {

			m.bind(ctx, i % 3);
			got = m.code(coder, bit, (u32)(i % m.table.width), i % 5);

		} else
			got = m.code(coder, bit, ctx, i % 5);

		const u64 salt{ i % 2 ? i % 3 : 0 };
		const u64 fallback{ cm::hash(salt * 8 + 7, 0) };
		require(m.table.keys[fallback & m.table.mask] == fallback);
		require(got == bit);

	}

}

template<int N>
void check(bool seeded, u32 width) {

	cm::model dynamic;
	cm::fixed_model<N> fixed;

	const auto init = [&] {

		// a small table deliberately collides, including between inputs bound to the same line.
		dynamic.init(N, 4, width, 5, true);
		fixed.init(4, width, 5, true);
		require(dynamic.weights.size() == 5 * (N + 1) && fixed.weights.size() == dynamic.weights.size());

		for (size_t i{}; i < dynamic.weights.size(); ++i) {

			const int initial{ i % (N + 1) == N ? 0 : 19661 };
			require(dynamic.weights[i] == initial && fixed.weights[i] == initial);

		}

		if (seeded) {

			seed(dynamic);
			seed(fixed);

		}

	};

	init();
	cm::bit_encoder expected, actual;
	run(dynamic, expected);
	run(fixed, actual);
	require(expected.finish() == actual.finish());
	equal(dynamic, fixed);

	fixed.init(4, width, 5, true);
	if (seeded)
		seed(fixed);
	cm::bit_decoder decoder{ expected.out };
	run(fixed, decoder);
	decoder.check_end();
	equal(dynamic, fixed);

	init();
	trainer reference, trained;
	run(dynamic, reference);
	run(fixed, trained);
	require(reference.seen == trained.seen);
	equal(dynamic, fixed);

}

int main() {
	for (bool seeded : { false, true })
		for (u32 width : { 1u, 8u, 32u, 64u }) {
			check<1>(seeded, width); check<2>(seeded, width); check<3>(seeded, width); check<4>(seeded, width);
			check<5>(seeded, width); check<6>(seeded, width); check<7>(seeded, width);
		}
}
