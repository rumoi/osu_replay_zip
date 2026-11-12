// clang++ -std=c++20 -O2 -I. -ILib tests/cm_counter_trainer.cpp -o /tmp/cm_counter_trainer && /tmp/cm_counter_trainer
// Counter-only fitting must preserve every observation and counter update, including aliased inputs.

#include <tuple>
#include "orz_shared.h"
#include "cm.h"

using namespace orz;

void require(bool ok) {
	if (!ok)
		throw std::runtime_error("counter-only trainer differs from full trainer");
}

template<typename Base>
struct observer : Base {

	std::vector<std::tuple<size_t, int, u16, u16, u64>> seen;

	void observe(const cm::line_table& table, size_t at, int bit) {
		seen.emplace_back(at, bit, table.t[at].p, table.t[at].n, table.keys[at / table.width]);
	}

};

template<typename Model>
constexpr int context_limit() {
	if constexpr (requires(Model m) { m.mixed(); })
		return 7; // the fallback model adds one input
	else
		return 8;
}

int main() {

	for (int inputs{ 1 }; inputs <= context_limit<cm::model>(); ++inputs) {
		for (int bits : { 0, 4 }) {

			cm::model full;
			full.init(inputs, bits, 4, 3, 5, true);

			for (size_t i{}; i < full.table.t.size(); ++i)
				full.table.t[i] = { (u16)(1024 + i * 487 % 63488), (u16)(i % 256) };

			for (size_t i{}; i < full.weights.size(); ++i)
				full.weights[i] += (int)(i % 7) * 1024;

			cm::model counters{ full };
			const auto weights{ counters.weights };
			const auto apm{ counters.refine.t };
			observer<cm::bit_trainer> reference;
			observer<cm::counter_trainer> candidate;
			u64 rng{ 127 };

			for (u32 i{}; i < 2048; ++i) {

				rng ^= rng << 13;
				rng ^= rng >> 7;
				rng ^= rng << 17;

				u64 ctx[cm::MAX_INPUTS];
				for (int j{}; j < inputs; ++j)
					ctx[j] = (rng >> (j * 3)) & 31;

				full.bind(ctx, i % 3);
				counters.bind(ctx, i % 3);

				const int y{ (int)(rng & 1) };
				require(full.code(reference, y, i % 4, i % 3, i % 5) == y);
				require(counters.code(candidate, y, i % 4, i % 3, i % 5) == y);

			}

			require(reference.seen == candidate.seen);
			require(full.table.keys == counters.table.keys);

			for (size_t i{}; i < full.table.t.size(); ++i)
				require(full.table.t[i].p == counters.table.t[i].p && full.table.t[i].n == counters.table.t[i].n);

			require(counters.weights == weights && counters.refine.t == apm);
			require(counters.refine.lo == 0 && counters.refine.wt == 0);

		}
	}

	return 0;
}
