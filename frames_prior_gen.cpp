// Generates frames_prior.h: the starting state of the context mixing models (orz_frames.h, orz_modes.h) for each prior
// set, fit to a list of replays. Not part of the orz build; compile it with the same headers and the LZMA SDK.
//
// usage: frames_prior_gen [--quick] [--keep] [--pooled] list.txt [entries] [sets at once] [cache directory] > frames_prior.h
//        frames_prior_gen --reuse [--quick] [--pooled] cache_directory [entries] [sets at once] > frames_prior.h
//   list.txt holds one replay per line: an .osr, or replay data without the .osr header in a file named
//   replay-<osu|taiko|fruits|mania>-*. With - for the list, the replays come from stdin instead, each as a mode byte
//   (an e_mode, relax included, or 255 for an .osr), its size in 4 bytes, then its bytes. entries caps the counters
//   of each set, those saving the most bits first; with --pooled it caps all sets' counters together, so the counters
//   go to whichever sets they save the most bits in.
//   --balanced and --fallback fit the counters of the balanced and the fastest frame profiles: the same exhaustive
//   counter starts, without mixer rounds, into frames_balanced_prior.h and frames_fallback_prior.h. --balanced
//   follows each model's first context and its fallback, --fallback the fallback alone in its two lines, where a
//   value's high byte is the counter's place in them (line * width + slot) and no key is stored.
//   Both encode with the default SIZE profile first, then fit conditional on those fixed decisions, and score a
//   counter by its own coding cost at every decision: a balanced decision is coded from the context's counter only
//   when its confidence is at least 8 (including seeded confidence), which the score does not follow.
//   Their kept/reused caches live in a balanced or fallback subdirectory, separate from normal and quick trials.
//   Compile with the same bootstrap headers when reusing encoded caches. --quick is not supported with either.
//   The current BALANCED format shares frames_prior.h. --balanced emits a research candidate bank, not a header
//   consumed by the runtime; adopting different counter values changes the format and requires matching encoders and decoders.
//   Sets are fit in parallel (default 4 at once). The encoded replays wait in the cache directory, a file per set
//   (default a temporary directory), and are read back for every pass, so memory does not grow with the replays.
//   --keep leaves them there (the directory must be given), with each set's candidate counters and the bits each
//   saves, and --reuse fits from such a directory: only pass 3 runs, so other entries or --pooled cost a fraction of
//   a fit.
//   --quick screens with confidences 1, 8 and 31 for both starting probabilities (7 trajectories instead of 19).
//   Coarser scores can change which counters are retained. Use the default exhaustive search for final priors.
//   Quick trials are saved separately as setN.quick.trials;
//   --reuse --quick reads those, while --reuse reads only exhaustive trials.
//   A fresh fit replaces the shared replay cache and discards both previous trial families.
//   Use separate cache directories to retain both searches' trials.
//
// Per set, over its replays:
//   1. train shared models on every replay; counters used in many replays are candidates, starting from their pooled
//      probability or from their trained state
//   2. run every replay again, following each candidate counter per replay from flat and from each start at each
//      confidence; the pair saving the most bits over starting flat is the counter's prior
//   3. code every replay from fresh models seeded with those counters, then average the mixer weights
//      over the replays where they are not at their flat start. Each round starts from the last one's averages.

#define _CRT_SECURE_NO_WARNINGS

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <string>
#include <string_view>
#include <thread>

#include "orz_shared.h"
#include "orz_write.h"
#include "orz_read.h"

using namespace orz;

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

namespace {

	constexpr u32 SETS{ frames::set_taiko + modes::mania + 1 };

	// candidates: counters seen this often, on the lines used by the most replays
	constexpr u32 MIN_COUNT{ 8 };
	constexpr size_t CANDIDATE_LINES{ 80000 };

	constexpr u16 CONFIDENCE[]{ 1, 2, 4, 6, 8, 12, 16, 24, 31 }; // up to cm::COUNTER_LIMIT
	constexpr int STARTS{ 2 };

	constexpr int MIX_ROUNDS{ 4 };

	// the counters a fit collects: every input's (the size profile), the first context's and the fallback's
	// (--balanced), or the fallback's alone in its two lines (--fallback)
	enum class _fit { normal, balanced, fallback };

	template<_fit Fit>
	using _collector = std::conditional_t<Fit == _fit::fallback, cm::fallback_counter_trainer,
		std::conditional_t<Fit == _fit::balanced, cm::balanced_counter_trainer, cm::counter_trainer>>;

	// the mode byte of a replay from stdin that is an .osr
	constexpr u8 OSR_RECORD{ 255 };

	using _models_list = std::array<cm::model*, FRAMES_PRIOR_MODELS>;

	// a replay's frames as the models take them
	struct _replay {

		size_t frames{};

		// osu!standard
		dspace::_model_info info;
		frames::_side side;
		frames::_frame_data fd;

		// other modes
		_osr R;
		modes::_prepared prepared;

	};

	// the prior set a replay trains, SETS for none (kept verbatim, or frames no set models). with_frames also decodes its
	// frames, as the models take them
	u32 decode(const std::vector<u8>& orz, _replay& r, bool with_frames) {

		_orz_file file{};

		if (!file.decode(orz) || (file.encode_info & ORZ_ENCODE_INFO::replay_verbatim))
			return SETS;

		const u32 flags{ file.encode_info };

		r.frames = file.frame_count;

		if (uses_modes_coder(get_e_mode(flags))) {

			const modes::kind mode{ modes::mode_of(get_e_mode(flags)) };

			if (!with_frames)
				return frames::set_taiko + mode;

			orz_state state{};
			std::vector<_osr_frame> frames;

			if (!orz::decode(orz, frames, state) || frames.size() < r.frames)
				return SETS;

			// without the closing seed frame
			r.R.assign(frames.begin(), frames.begin() + r.frames);
			r.prepared = modes::prepare(r.R, mode, (flags & ORZ_ENCODE_INFO::text_shortest) != 0);

			return frames::set_taiko + mode;
		}

		std::span<const u8> data;

		for (const auto& c : file.container)
			if (c.tag == orz_tag::frames)
				data = c.data;

		standard::_frames_head head{};

		if (!standard::read_head(data, get_e_mode(flags) == e_mode::osu_autopilot, head))
			return SETS;

		r.info = std::move(head.info);

		r.side.axis = r.info.axis;
		r.side.table = std::move(head.table);
		r.side.grid = head.grid;
		r.side.mode = get_e_mode(flags);
		r.side.ap = head.ap;
		r.side.std_default_time = head.std_default_time;

		const u32 set{ frames::prior_set(r.side) };

		if (with_frames) {

			frames::_models models{};

			switch (file.frame_profile()) {
			case _frame_profile::speed: { frames::_fastest_reader reader{ models, head.rest, r.fd, r.side, r.frames }; } break;
			case _frame_profile::balanced: { frames::_balanced_reader reader{ models, head.rest, r.fd, r.side, r.frames }; } break;
			default: { frames::_reader reader{ models, head.rest, r.fd, r.side, r.frames }; }
			}

		}

		return set;
	}

	// one set's models, at training size or for a replay
	struct _set_models {

		frames::_models standard;
		modes::_catch_models fruit;
		modes::_models other{ standard, fruit };

		void init(u32 set, size_t frames, bool training, bool fallback = false) {

			if (set < frames::set_taiko)
				standard.init(frames, set >= frames::set_relax_rational && set <= frames::set_relax_other ? e_mode::osu_relax : e_mode::osu, training, fallback);
			else
				other.init(frames, modes::kind(set - frames::set_taiko), training, fallback);

		}

		_models_list all(u32 set) {
			return set < frames::set_taiko ? standard.all() : other.all();
		}

		template<typename Coder>
		void run(Coder& c, u32 set, _replay& r) {

			if (set < frames::set_taiko)
				frames::frame_loop(c, standard, r.fd, r.side, r.frames, true);
			else
				modes::frame_loop(c, other, r.R, r.prepared.escaped, r.prepared.speeds, r.prepared.side, true);

		}

	};

	// a set's encoded replays in list order, each as its size then its bytes
	std::filesystem::path cache_file(const std::filesystem::path& cache, u32 set) {
		return cache / ("set" + std::to_string(set) + ".orz");
	}

	// a set's replays decoded one at a time, as their decoded frames together would not fit in memory
	template<typename F>
	void each_replay(const std::filesystem::path& cache, u32 set, F&& f) {

		FILE* file{ fopen(cache_file(cache, set).string().c_str(), "rb") };

		if (!file)
			return;

		std::vector<u8> orz;

		for (u32 size; fread(&size, sizeof size, 1, file) == 1;) {

			orz.resize(size);

			if (fread(orz.data(), 1, size, file) != size)
				break;

			_replay r;

			if (decode(orz, r, true) == set)
				f(r);

		}

		fclose(file);
	}

	// model index of a table in the list
	u32 model_of(const _models_list& models, const cm::line_table& table) {

		u32 m{};

		while (m < FRAMES_PRIOR_MODELS && !(models[m] && &models[m]->table == &table))
			++m;

		return m;
	}

	// pass 1: per counter the bits seen, per line the replays that used it
	template<_fit Fit>
	struct _counting : _collector<Fit> {

		struct _table {
			std::vector<u64> ones, seen; // per counter
			std::vector<u32> replays, last; // per line
		};

		_models_list models{};
		_table tables[FRAMES_PRIOR_MODELS];
		u32 replay{};

		void observe(const cm::line_table& table, size_t at, int y) {

			const u32 m{ model_of(models, table) };

			if (m == FRAMES_PRIOR_MODELS)
				return;

			_table& s{ tables[m] };
			const size_t line{ at / table.width };

			++s.seen[at];
			s.ones[at] += (u32)y;

			if (s.last[line] != replay) {

				s.last[line] = replay;
				++s.replays[line];

			}

		}

	};

	template<bool Quick>
	struct _candidate {

		static constexpr int CONFIDENCE_STEP{ Quick ? 4 : 1 };
		static constexpr int CONFIDENCES{ ((int)std::size(CONFIDENCE) - 1) / CONFIDENCE_STEP + 1 };
		static constexpr int TRIALS{ STARTS * CONFIDENCES + 1 }; // then flat

		u32 model, slot;
		u64 key;
		u16 start[STARTS]; // pooled, trained
		u32 replay{ ~0u };
		cm::counter trial[TRIALS];
		double bits[TRIALS]{};
	};

	// log2(p / 65536) per 16-bit probability p, looked up as the trials take it for every bit of every candidate
	const auto LOG2{ [] {

		std::vector<double> l(65537);

		for (int p{ 1 }; p <= 65536; ++p)
			l[p] = std::log2(p / 65536.0);

		return l;
	}() };

	// pass 2: follows the candidates through each replay from both starts, each searched confidence and flat
	template<bool Quick, _fit Fit>
	struct _trials : _collector<Fit> {

		using candidate = _candidate<Quick>;
		static constexpr int CONFIDENCES{ candidate::CONFIDENCES }, TRIALS{ candidate::TRIALS };

		_models_list models{};
		std::vector<candidate> candidates;
		std::vector<i32> row[FRAMES_PRIOR_MODELS]; // per line: where its counters start in slots, -1 for no candidates
		std::vector<i32> slots; // per counter of a candidate line: its candidate, -1 for none
		u32 replay{};

		void observe(const cm::line_table& table, size_t at, int y) {

			const u32 m{ model_of(models, table) };

			if (m == FRAMES_PRIOR_MODELS)
				return;

			const size_t line{ at / table.width };
			const i32 r{ row[m][line] };

			if (r < 0)
				return;

			const i32 i{ slots[r + at % table.width] };

			if (i < 0 || candidates[i].key != table.keys[line])
				return;

			candidate& c{ candidates[i] };

			if (c.replay != replay) {

				c.replay = replay;

				for (int s{}; s < STARTS; ++s)
					for (int k{}; k < CONFIDENCES; ++k)
						c.trial[s * CONFIDENCES + k] = cm::counter::of(c.start[s], CONFIDENCE[k * candidate::CONFIDENCE_STEP]);

				c.trial[TRIALS - 1] = {};

			}

			for (int t{}; t < TRIALS; ++t) {

				const int p{ std::clamp(c.trial[t].p(), 32, 65504) };

				c.bits[t] -= LOG2[y ? p : 65536 - p];
				cm::update(c.trial[t], y);

			}

		}

	};

	struct _set_prior {
		std::vector<u32> start, weight_start;
		std::vector<u32> keys, values;
		std::vector<i32> weights;
	};

	// a candidate counter's best start: the bits it saves over starting flat, and its frames_prior.h entry
	struct _pick {
		double gain;
		u32 model, key, value;
	};

	// passes 1 and 2 of a set: its candidates that save bits, in candidate order
	struct _set_trials {
		size_t replays{};
		std::vector<_pick> picks;
	};

	frames::_prior view(const _set_prior& p) {
		return { p.start.data(), p.keys.data(), p.values.data(), p.weight_start.data(), p.weights.data() };
	}

	template<bool Quick, _fit Fit = _fit::normal>
	_set_trials trials_of(const std::filesystem::path& cache, u32 set) {

		using candidate = _candidate<Quick>;
		constexpr int CONFIDENCES{ candidate::CONFIDENCES }, TRIALS{ candidate::TRIALS };

		_set_trials out{};

		// 1. shared models over every replay
		std::vector<candidate> candidates;

		{
			_set_models M{};
			_counting<Fit> counting{};

			M.init(set, size_t(1) << 20, true, Fit == _fit::fallback);
			counting.models = M.all(set);

			for (u32 m{}; m < FRAMES_PRIOR_MODELS; ++m) {

				const cm::model* md{ counting.models[m] };

				if (!md || md->table.t.empty())
					continue;

				auto& s{ counting.tables[m] };

				s.ones.assign(md->table.t.size(), 0);
				s.seen.assign(md->table.t.size(), 0);
				s.replays.assign(md->table.keys.size(), 0);
				s.last.assign(md->table.keys.size(), ~0u);

			}

			each_replay(cache, set, [&](_replay& r) {

				M.run(counting, set, r);

				++counting.replay;
				++out.replays;

			});

			struct _line {
				u64 rank;
				u32 model;
				size_t index;
			};

			std::vector<_line> lines;

			// by the replays using the line, then its uses
			for (u32 m{}; m < FRAMES_PRIOR_MODELS; ++m) {

				const auto& s{ counting.tables[m] };

				for (size_t i{}; i < s.replays.size(); ++i) {

					if (!s.replays[i])
						continue;

					const u32 width{ counting.models[m]->table.width };
					const u64 uses{ std::accumulate(s.seen.begin() + i * width, s.seen.begin() + (i + 1) * width, u64{}) };

					lines.push_back({ (u64)s.replays[i] << 32 | std::min<u64>(uses, 0xFFFFFFFF), m, i });

				}

			}

			const size_t keep{ std::min(lines.size(), CANDIDATE_LINES) };

			// ties by model, then line, so the result does not depend on the sort's implementation
			std::partial_sort(lines.begin(), lines.begin() + keep, lines.end(), [](const _line& a, const _line& b) {
				return a.rank != b.rank ? a.rank > b.rank : a.model != b.model ? a.model < b.model : a.index < b.index;
			});
			lines.resize(keep);

			for (const auto& l : lines) {

				const auto& table{ counting.models[l.model]->table };
				const auto& s{ counting.tables[l.model] };

				for (u32 slot{}; slot < table.width; ++slot) {

					const size_t at{ l.index * table.width + slot };

					if (s.seen[at] < MIN_COUNT)
						continue;

					candidate c{ l.model, slot, table.keys[l.index] };

					c.start[0] = (u16)std::clamp<i64>(std::llround((s.ones[at] + 0.5) * 65536.0 / (s.seen[at] + 1.0)), 32, 65503);
					c.start[1] = (u16)table.t[at].p();

					candidates.push_back(c);

				}

			}

		}

		// 2. each candidate per replay, from flat and from each start and searched confidence
		_trials<Quick, Fit> trials{};

		{
			_set_models M{};

			M.init(set, size_t(1) << 20, true, Fit == _fit::fallback);
			trials.models = M.all(set);
			trials.candidates = std::move(candidates);

			for (u32 m{}; m < FRAMES_PRIOR_MODELS; ++m)
				if (trials.models[m])
					trials.row[m].assign(trials.models[m]->table.keys.size(), -1);

			for (size_t i{}; i < trials.candidates.size(); ++i) {

				const auto& c{ trials.candidates[i] };
				const auto& table{ trials.models[c.model]->table };
				// Compact fallback lines are stored by salt, rather than by the masked hash.
				const size_t line{ Fit == _fit::fallback ? (c.key == cm::hash(cm::FALLBACK_INPUT, 0) ? 0u : 1u) : c.key & table.mask };
				i32& r{ trials.row[c.model][line] };

				if (r < 0) {

					r = (i32)trials.slots.size();
					trials.slots.resize(trials.slots.size() + table.width, -1);

				}

				trials.slots[r + c.slot] = (i32)i;

			}

			each_replay(cache, set, [&](_replay& r) {

				M.run(trials, set, r);

				++trials.replay;

			});

		}

		for (const auto& c : trials.candidates) {

			u32 best{ TRIALS - 1 };

			for (u32 t{}; t < TRIALS - 1; ++t)
				if (c.bits[t] < c.bits[best])
					best = t;

			if (best == TRIALS - 1)
				continue;

			const u32 confidence{ CONFIDENCE[(best % CONFIDENCES) * candidate::CONFIDENCE_STEP] };

			out.picks.push_back({ c.bits[TRIALS - 1] - c.bits[best], c.model, (u32)c.key, c.start[best / CONFIDENCES] | confidence << 16 | c.slot << 24 });

		}

		return out;
	}

	// a set's trials kept in the cache directory, for --reuse by the same build: the replays and pick count, then the picks
	std::filesystem::path trials_file(const std::filesystem::path& cache, u32 set, bool quick) {
		return cache / ("set" + std::to_string(set) + (quick ? ".quick.trials" : ".trials"));
	}

	bool save_trials(const std::filesystem::path& cache, u32 set, const _set_trials& t, bool quick) {

		FILE* file{ fopen(trials_file(cache, set, quick).string().c_str(), "wb") };

		if (!file)
			return false;

		const u64 head[2]{ t.replays, t.picks.size() };
		const bool ok{ fwrite(head, sizeof head, 1, file) == 1 && fwrite(t.picks.data(), sizeof(_pick), t.picks.size(), file) == t.picks.size() };

		return fclose(file) == 0 && ok;
	}

	bool load_trials(const std::filesystem::path& cache, u32 set, _set_trials& t, bool quick) {

		FILE* file{ fopen(trials_file(cache, set, quick).string().c_str(), "rb") };

		if (!file)
			return false;

		u64 head[2];
		bool ok{ fread(head, sizeof head, 1, file) == 1 };

		if (ok) {

			t.replays = head[0];
			t.picks.resize(head[1]);

			ok = fread(t.picks.data(), sizeof(_pick), t.picks.size(), file) == t.picks.size();

		}

		fclose(file);

		return ok;
	}

	// 3. a set's prior from the picks it keeps (indices into t.picks, most bits saved first)
	_set_prior mix(const std::filesystem::path& cache, u32 set, const _set_trials& t, const std::vector<u32>& kept, _fit fit = _fit::normal) {

		_set_prior out{};

		// by model, most bits saved first
		std::vector<std::pair<u32, u32>> counters[FRAMES_PRIOR_MODELS];

		for (const u32 i : kept)
			counters[t.picks[i].model].push_back({ t.picks[i].key, t.picks[i].value });

		out.start.push_back(0);

		for (u32 m{}; m < FRAMES_PRIOR_MODELS; ++m) {

			for (const auto& [key, value] : counters[m]) {

				out.keys.push_back(key);
				out.values.push_back(value);

			}

			out.start.push_back((u32)out.keys.size());

		}

		out.weight_start.assign(FRAMES_PRIOR_MODELS + 1, 0);

		// the balanced and fallback profiles have no mixer
		if (fit != _fit::normal) {

			if (fit == _fit::fallback) {

				// a value's high byte becomes the counter's place in its model's two lines, which stands for its key
				_set_models M{};

				M.init(set, 1, false, true);

				const auto models{ M.all(set) };
				const u32 first_line{ (u32)cm::hash(cm::FALLBACK_INPUT, 0) };

				for (u32 m{}; m < FRAMES_PRIOR_MODELS; ++m) {

					for (u32 j{ out.start[m] }; j < out.start[m + 1]; ++j) {

						const u32 at{ (out.keys[j] == first_line ? 0u : models[m]->table.width) + (out.values[j] >> 24) };

						if (at >= models[m]->table.t.size() || at > 255) {

							fprintf(stderr, "set %u: a fallback counter of model %u is outside its lines\n", set, m);
							std::exit(1);

						}

						out.values[j] = (out.values[j] & 0xFFFFFF) | at << 24;

					}

				}

			}

			fprintf(stderr, "set %u: %zu replays, %zu %s counters\n", set, t.replays, out.keys.size(), fit == _fit::fallback ? "fallback" : "balanced");

			return out;
		}

		// 3. mixer weights, averaged over the replays where they are not at their flat start
		for (int round{}; round < MIX_ROUNDS; ++round) {

			std::vector<double> weights[FRAMES_PRIOR_MODELS];
			std::vector<u32> weights_n[FRAMES_PRIOR_MODELS];
			size_t inputs[FRAMES_PRIOR_MODELS]{};

			each_replay(cache, set, [&](_replay& r) {

				_set_models M{};
				cm::bit_trainer t{};

				M.init(set, r.frames, false);

				const auto models{ M.all(set) };

				frames::seed(models, view(out));
				M.run(t, set, r);

				for (u32 m{}; m < FRAMES_PRIOR_MODELS; ++m) {

					const cm::model* md{ models[m] };

					if (!md || md->table.t.empty())
						continue;

					const size_t n{ (size_t)md->mixed() }, sets{ md->weights.size() / n };

					if (weights[m].empty()) {

						inputs[m] = n;

						weights[m].assign(md->weights.size(), 0);
						weights_n[m].assign(sets, 0);

					}

					for (size_t s{}; s < sets; ++s) {

						const auto w{ md->weights.begin() + s * n };

						bool moved{};

						for (size_t i{}; i < n; ++i)
							moved |= w[i] != cm::start_weight(i, n);

						if (!moved)
							continue;

						++weights_n[m][s];

						for (size_t i{}; i < n; ++i)
							weights[m][s * n + i] += w[i];

					}

				}

			});

			out.weights.clear();

			for (u32 m{}; m < FRAMES_PRIOR_MODELS; ++m) {

				out.weight_start[m] = (u32)out.weights.size();

				for (size_t i{}; i < weights[m].size(); ++i) {

					const u32 n{ weights_n[m][i / inputs[m]] };

					out.weights.push_back(n ? (i32)std::lround(weights[m][i] / n) : cm::start_weight(i % inputs[m], inputs[m]));

				}

			}

			out.weight_start[FRAMES_PRIOR_MODELS] = (u32)out.weights.size();

		}

		fprintf(stderr, "set %u: %zu replays, %zu counters\n", set, t.replays, out.keys.size());

		return out;
	}

	// encode info for a replay file: .osr carries the mode, headerless data names it
	u32 flags_of(const std::string& path) {

		constexpr const char* NAMES[]{ "replay-osu-", "replay-taiko-", "replay-fruits-", "replay-mania-" };

		const std::string name{ std::filesystem::path(path).filename().string() };

		for (u32 m{}; m < 4; ++m)
			if (name.starts_with(NAMES[m]))
				return m;

		return ORZ_ENCODE_INFO::has_osr_header;
	}

	template<typename T>
	void print_array(const char* type, const char* name, const std::vector<T>& v, int per_line) {

		printf("constexpr %s %s[%zu]{", type, name, std::max<size_t>(v.size(), 1));

		for (size_t i{}; i < v.size(); ++i) {

			if constexpr (std::is_same_v<T, u32>)
				printf("%s0x%x,", i % per_line ? " " : "\n\t", v[i]);
			else
				printf("%s%d,", i % per_line ? " " : "\n\t", (int)v[i]);

		}

		printf("%s\n};\n\n", v.empty() ? " 0" : "");

	}

	void print_starts(const char* name, const _set_prior (&sets)[SETS], std::vector<u32> _set_prior::* starts, const size_t (&base)[SETS], const char* prefix = "FRAMES_PRIOR") {

		printf("constexpr uint32_t %s[%s_SETS][%s_MODELS + 1]{\n", name, prefix, prefix);

		for (u32 s{}; s < SETS; ++s) {

			printf("\t{");

			for (u32 m{}; m <= FRAMES_PRIOR_MODELS; ++m)
				printf(" %zu%s", base[s] + (sets[s].*starts)[m], m < FRAMES_PRIOR_MODELS ? "," : " ");

			printf("},\n");

		}

		printf("};\n\n");

	}

}

int main(int argc, char** argv) {

	bool keep{}, reuse{}, pooled{}, quick{}, balanced{}, fallback{};
	std::vector<const char*> args;

	for (int i{ 1 }; i < argc; ++i) {

		const std::string_view arg{ argv[i] };

		if (arg == "--balanced")
			balanced = true;
		else if (arg == "--fallback")
			fallback = true;
		else if (arg == "--quick")
			quick = true;
		else if (arg == "--keep")
			keep = true;
		else if (arg == "--reuse")
			reuse = true;
		else if (arg == "--pooled")
			pooled = true;
		else
			args.push_back(argv[i]);

	}

	if (args.empty() || (keep && (reuse || args.size() < 4)) || ((balanced || fallback) && quick) || (balanced && fallback)) {

		fprintf(stderr, "usage: frames_prior_gen [--quick] [--keep] [--pooled] list.txt [entries] [sets at once] [cache directory] > frames_prior.h\n"
			"       frames_prior_gen --reuse [--quick] [--pooled] cache_directory [entries] [sets at once] > frames_prior.h\n"
			"       add --balanced or --fallback (without --quick) to generate frames_balanced_prior.h or frames_fallback_prior.h;\n"
			"       their caches use cache_directory/balanced and cache_directory/fallback\n");

		return 1;
	}

	const size_t entries{ args.size() > 1 ? (size_t)atoll(args[1]) : 36000 };
	const u32 at_once{ args.size() > 2 ? (u32)std::max(1, atoi(args[2])) : 4u };

	// the list's replays are encoded into the cache unless it is reused, and removed at the end unless kept
	const bool temporary{ !reuse && args.size() < 4 };

	const std::filesystem::path cache_base{ reuse ? args[0] : !temporary ? args[3] : std::filesystem::temp_directory_path() /
		("frames_prior_gen-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())) };
	const _fit fit{ balanced ? _fit::balanced : fallback ? _fit::fallback : _fit::normal };
	const std::filesystem::path cache{ balanced ? cache_base / "balanced" : fallback ? cache_base / "fallback" : cache_base };

	std::error_code error;

	std::filesystem::create_directories(cache, error);

	const bool from_stdin{ !reuse && std::string_view(args[0]) == "-" };

	std::vector<std::string> paths;

	if (!reuse && !from_stdin) {

		std::ifstream list(args[0]);
		std::string path;

		while (std::getline(list, path))
			if (!path.empty())
				paths.push_back(path);

	}

	// a replay from stdin: false at the end of the stream
	const auto read_record = [](std::vector<u8>& data, u8& mode) {

		u32 size;

		if (fread(&mode, 1, 1, stdin) != 1 || fread(&size, sizeof size, 1, stdin) != 1)
			return false;

		data.resize(size);

		return fread(data.data(), 1, size, stdin) == size;
	};

	// encoded once, in parallel a chunk at a time, and kept in their sets' cache files in list order
	if (!reuse) {

		// Both searches share the replay cache. Invalidate both trial families before replacing any of it.
		for (u32 s{}; s < SETS; ++s)
			for (bool quick_trials : { false, true }) {

				const auto path{ trials_file(cache, s, quick_trials) };
				std::filesystem::remove(path, error);

				if (error) {

					fprintf(stderr, "can not remove %s\n", path.string().c_str());
					return 1;

				}

			}

		size_t replays{};
		FILE* files[SETS]{};

		for (u32 s{}; s < SETS; ++s) {

			if (!(files[s] = fopen(cache_file(cache, s).string().c_str(), "wb"))) {

				fprintf(stderr, "can not write %s\n", cache_file(cache, s).string().c_str());

				return 1;
			}

		}

		constexpr size_t CHUNK{ 1024 };

		std::vector<std::vector<u8>> records(CHUNK), encoded(CHUNK);
		u8 modes[CHUNK]{};
		u32 set_of[CHUNK]{};

		for (size_t base{};; base += CHUNK) {

			size_t n{};

			if (from_stdin)
				while (n < CHUNK && read_record(records[n], modes[n]))
					++n;
			else
				n = std::min(CHUNK, paths.size() - std::min(base, paths.size()));

			if (n == 0)
				break;

			std::atomic<size_t> next{};
			std::vector<std::thread> workers;

			for (u32 w{}; w < std::max(1u, std::thread::hardware_concurrency()); ++w)
				workers.emplace_back([&] {

					for (size_t i; (i = next++) < n;) {

						orz_state state{};
						_replay r;

						if (!from_stdin) {

							const u32 f{ flags_of(paths[base + i]) };

							records[i] = read_file(paths[base + i].c_str());
							modes[i] = (f & ORZ_ENCODE_INFO::has_osr_header) ? OSR_RECORD : (u8)get_e_mode(f);

						}

						encoded[i].clear();

						if (modes[i] == OSR_RECORD)
							osr_to_orz(records[i], encoded[i], state);
						else if (modes[i] < (u32)e_mode::e_mode_max)
							replay_data_to_orz(records[i], (e_mode)modes[i], encoded[i], state);

						set_of[i] = decode(encoded[i], r, false);

					}

				});

			for (auto& w : workers)
				w.join();

			for (size_t i{}; i < n; ++i) {

				if (set_of[i] == SETS)
					continue;

				const u32 size{ (u32)encoded[i].size() };

				fwrite(&size, sizeof size, 1, files[set_of[i]]);
				fwrite(encoded[i].data(), 1, size, files[set_of[i]]);

			}

			replays += n;

		}

		for (FILE* f : files)
			fclose(f);

		fprintf(stderr, "encoded %zu replays\n", replays);

	}

	// f(set) for every set, at_once in parallel
	const auto each_set = [&](auto&& f) {

		std::atomic<u32> next{};
		std::vector<std::thread> workers;

		for (u32 w{}; w < std::min(at_once, SETS); ++w)
			workers.emplace_back([&] {

				for (u32 s; (s = next++) < SETS;)
					f(s);

			});

		for (auto& w : workers)
			w.join();

	};

	_set_trials trials[SETS];
	std::atomic<bool> failed{};

	each_set([&](u32 s) {

		if (!reuse)
			trials[s] = balanced ? trials_of<false, _fit::balanced>(cache, s) : fallback ? trials_of<false, _fit::fallback>(cache, s) :
				quick ? trials_of<true>(cache, s) : trials_of<false>(cache, s);

		if (reuse ? !load_trials(cache, s, trials[s], quick) : keep && !save_trials(cache, s, trials[s], quick)) {

			fprintf(stderr, "can not %s %s\n", reuse ? "read" : "write", trials_file(cache, s, quick).string().c_str());

			failed = true;

		}

	});

	if (failed)
		return 1;

	// the picks each set keeps, most bits saved first: up to entries per set, or with --pooled across all of them
	std::vector<u32> kept[SETS];

	{
		struct _rank {
			double gain;
			u32 set, pick;
		};

		std::vector<_rank> ranks;

		for (u32 s{}; s < SETS; ++s)
			for (u32 i{}; i < trials[s].picks.size(); ++i)
				ranks.push_back({ trials[s].picks[i].gain, s, i });

		std::sort(ranks.begin(), ranks.end(), [](const _rank& a, const _rank& b) {
			return a.gain != b.gain ? a.gain > b.gain : a.set != b.set ? a.set < b.set : a.pick < b.pick;
		});

		size_t total{};

		for (const auto& r : ranks) {

			if (pooled ? total < entries : kept[r.set].size() < entries) {

				kept[r.set].push_back(r.pick);
				++total;

			}

		}

	}

	_set_prior sets[SETS];

	each_set([&](u32 s) {
		sets[s] = mix(cache, s, trials[s], kept[s], fit);
	});

	if (!keep && !reuse)
		for (u32 s{}; s < SETS; ++s)
			std::filesystem::remove(cache_file(cache, s), error);

	if (temporary) {
		std::filesystem::remove(cache, error);
		if (fit != _fit::normal)
			std::filesystem::remove(cache_base, error);
	}

	std::vector<u32> keys, values;
	std::vector<i32> weights;
	size_t key_base[SETS]{}, weight_base[SETS]{};

	for (u32 s{}; s < SETS; ++s) {

		key_base[s] = keys.size();
		weight_base[s] = weights.size();

		keys.insert(keys.end(), sets[s].keys.begin(), sets[s].keys.end());
		values.insert(values.end(), sets[s].values.begin(), sets[s].values.end());
		weights.insert(weights.end(), sets[s].weights.begin(), sets[s].weights.end());

	}

	printf("#pragma once\n\n#include <cstdint>\n\n");

	if (balanced) {
		printf("// Generated by frames_prior_gen --balanced: exhaustive pooled/trained counter starts, nine confidences plus flat.\n");
		printf("// The first context's counters and the fallback's, conditional on the cached representations;\n");
		printf("// no mixer state. Key = low 32 bits of the context hash; value = p | confidence << 16 | slot << 24.\n\n");
		printf("constexpr uint32_t BALANCED_PRIOR_SETS{ %u };\n", SETS);
		printf("constexpr uint32_t BALANCED_PRIOR_MODELS{ %u };\n\n", FRAMES_PRIOR_MODELS);
		print_starts("BALANCED_PRIOR_START", sets, &_set_prior::start, key_base, "BALANCED_PRIOR");
		print_array("uint32_t", "BALANCED_PRIOR_KEYS", keys, 8);
		print_array("uint32_t", "BALANCED_PRIOR_VALUES", values, 8);
		return 0;
	}

	if (fallback) {
		printf("// Generated by frames_prior_gen --fallback: exhaustive pooled/trained counter starts, nine confidences plus flat.\n");
		printf("// Conditional on the cached representations; no mixer state.\n");
		printf("// Value = p | confidence << 16 | (line * width + slot) << 24: the counter's place in its model's two lines.\n\n");
		printf("constexpr uint32_t FALLBACK_PRIOR_SETS{ %u };\n", SETS);
		printf("constexpr uint32_t FALLBACK_PRIOR_MODELS{ %u };\n\n", FRAMES_PRIOR_MODELS);
		print_starts("FALLBACK_PRIOR_START", sets, &_set_prior::start, key_base, "FALLBACK_PRIOR");
		print_array("uint32_t", "FALLBACK_PRIOR_VALUES", values, 8);
		return 0;
	}

	if (quick)
		printf("// quick screening output: confidences 1/8/31, %d mixer rounds; use an exhaustive fit for final priors\n\n", MIX_ROUNDS);

	printf("// generated by frames_prior_gen.cpp: the starting state of the context mixing models per prior set (frames::prior_set)\n");
	printf("// and model (frames::_models::all, modes::_models::all). Counters are keyed by the low 32 bits of their context hash,\n");
	printf("// value = p | confidence << 16 | slot << 24; then each model's mixer weights\n\n");
	printf("constexpr uint32_t FRAMES_PRIOR_SETS{ %u };\n", SETS);
	printf("constexpr uint32_t FRAMES_PRIOR_MODELS{ %u };\n\n", FRAMES_PRIOR_MODELS);

	print_starts("FRAMES_PRIOR_START", sets, &_set_prior::start, key_base);
	print_starts("FRAMES_PRIOR_WEIGHT_START", sets, &_set_prior::weight_start, weight_base);

	print_array("uint32_t", "FRAMES_PRIOR_KEYS", keys, 8);
	print_array("uint32_t", "FRAMES_PRIOR_VALUES", values, 8);
	print_array("int32_t", "FRAMES_PRIOR_WEIGHTS", weights, 16);

	return 0;
}
