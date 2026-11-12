// clang++ -std=c++20 -O2 -I. -ILib tests/cm_fixed_model.cpp -o /tmp/cm_fixed_model && /tmp/cm_fixed_model
// fixed input counts must preserve the adaptive state, coded bytes and prior trainer's observations.

#include "orz_shared.h"
#include "cm.h"

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

	require(a.table.keys == b.table.keys && a.weights == b.weights && a.refine.t == b.refine.t);

}

struct trainer : cm::bit_trainer {

	std::vector<std::pair<size_t, int>> seen;

	void observe(const cm::line_table&, size_t at, int bit) {
		seen.emplace_back(at, bit);
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

		if (i % 2) {

			m.bind(ctx, i % 3);
			got = m.code(coder, bit, (u32)(i % 8), i % 5, i % 3);

		} else
			got = m.code(coder, bit, ctx, i % 5, i % 3);

		require(got == bit);

	}

}

template<int N>
void check() {

	cm::model dynamic;
	cm::fixed_model<N> fixed;

	const auto init = [&] {

		// a small table deliberately collides, including between inputs bound to the same line.
		dynamic.init(N, 4, 8, 5, 3, true);
		fixed.init(4, 8, 5, 3, true);
		seed(dynamic);
		seed(fixed);

	};

	init();
	cm::bit_encoder expected, actual;
	run(dynamic, expected);
	run(fixed, actual);
	require(expected.finish() == actual.finish());
	equal(dynamic, fixed);

	fixed.init(4, 8, 5, 3, true);
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
	check<1>(); check<2>(); check<3>(); check<4>(); check<5>(); check<6>(); check<7>(); check<8>();
}
