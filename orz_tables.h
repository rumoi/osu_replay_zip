#pragma once

#include "orz_frames.h"

namespace orz {

namespace modes {

	enum kind : u32 { taiko, fruits, mania };

	// catcher residual decisions: 0 zero, 1 sign, 2 length gate, 3-34 length up, 35-63 length down
	constexpr u32 RES_STEPS{ 64 };

	struct _models {

		frames::_models keys_time;
		cm::fixed_model<6> moved;
		cm::fixed_model<4> dir;
		cm::fixed_model<5> res;
		cm::fixed_model<2> res_bits;

		void init(size_t frames, kind mode, bool record_keys = false, bool fallback_only = false) {

			keys_time.init_keys_time(frames, mode == taiko, record_keys, fallback_only);

			if (mode != fruits)
				return;

			const int b{ (int)std::bit_width(std::min(frames, frames::TABLE_FRAMES)) }, extra{ record_keys ? 4 : 0 };

			moved.init(std::clamp(b + 3, 12, 20 + extra), 1, 64, record_keys, fallback_only);
			dir.init(std::clamp(b + 1, 10, 18 + extra), 1, 16, record_keys, fallback_only);
			res.init(std::clamp(b, 10, record_keys ? 18 + extra : 14), RES_STEPS, RES_STEPS, record_keys, fallback_only);
			res_bits.init(std::clamp(b + 1, 10, 18 + extra), 1, 8, record_keys, fallback_only);

		}

		// in frames_prior.h order
		std::array<cm::model*, FRAMES_PRIOR_MODELS> all() {
			return { &keys_time.keys_changed, &keys_time.keys_bits, &keys_time.keys_high, &keys_time.time, &moved, &dir, &res, &res_bits };
		}

		void seed(kind mode, bool fallback_only = false, bool balanced = false) {
			if (fallback_only)
				frames::seed_fallback(all(), frames::set_taiko + mode);
			else
				frames::seed(all(), frames::builtin_prior(frames::set_taiko + mode), balanced);
		}

	};

}

// the coders' context mixing tables, kept between replays
struct _table_memory {

	frames::_models standard;
	modes::_models modes;

};

}
