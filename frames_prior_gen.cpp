// Generates frames_prior.h: the starting state of the context mixing models (orz_frames.h, orz_modes.h) for each prior
// set, fit to a list of replays. Not part of the orz build; compile it with the same headers and the LZMA SDK.
//
// usage: frames_prior_gen list.txt [entries] [sets at once] [cache directory] > frames_prior.h
//   list.txt holds one replay per line: an .osr, or replay data without the .osr header in a file named
//   replay-<osu|taiko|fruits|mania>-*. With - for the list, the replays come from stdin instead, each as a mode byte
//   (an e_mode, relax included, or 255 for an .osr), its size in 4 bytes, then its bytes. entries caps the counters
//   of each set, those saving the most bits first.
//   Sets are fit in parallel (default 4 at once). The encoded replays wait in the cache directory, a file per set
//   (default a temporary directory), and are read back for every pass, so memory does not grow with the replays.
//
// Per set, over its replays:
//   1. train shared models on every replay; counters used in many replays are candidates, starting from their pooled
//      probability or from their trained state
//   2. run every replay again, following each candidate counter per replay from flat and from each start at each
//      confidence; the pair saving the most bits over starting flat is the counter's prior
//   3. code every replay from fresh models seeded with those counters, then average the mixer weights and apm tables
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

	constexpr u16 CONFIDENCE[]{ 1, 2, 4, 8, 16, 32, 64, 128, 255 };
	constexpr int STARTS{ 2 }, CONFIDENCES{ (int)std::size(CONFIDENCE) }, TRIALS{ STARTS * CONFIDENCES + 1 }; // then flat

	constexpr int MIX_ROUNDS{ 4 };

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

			_decoded_orz d;

			if (!orz_decode_frames(orz, d) || d.frames.size() < r.frames)
				return SETS;

			// without the closing seed frame
			r.R.assign(d.frames.begin(), d.frames.begin() + r.frames);
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

			frames::_reader reader{ models, head.rest, r.fd, r.side, r.frames };

		}

		return set;
	}

	// one set's models, at training size or for a replay
	struct _set_models {

		frames::_models standard;
		modes::_models other;

		void init(u32 set, size_t frames, bool training) {

			if (set < frames::set_taiko)
				standard.init(frames, set >= frames::set_relax_rational && set <= frames::set_relax_other ? e_mode::osu_relax : e_mode::osu, training);
			else
				other.init(frames, modes::kind(set - frames::set_taiko), training);

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
	struct _counting : cm::bit_trainer {

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

	struct _candidate {
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

	// pass 2: follows the candidates through each replay from every start, confidence and flat
	struct _trials : cm::bit_trainer {

		_models_list models{};
		std::vector<_candidate> candidates;
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

			_candidate& c{ candidates[i] };

			if (c.replay != replay) {

				c.replay = replay;

				for (int s{}; s < STARTS; ++s)
					for (int k{}; k < CONFIDENCES; ++k)
						c.trial[s * CONFIDENCES + k] = { c.start[s], CONFIDENCE[k] };

				c.trial[TRIALS - 1] = {};

			}

			for (int t{}; t < TRIALS; ++t) {

				const int p{ std::clamp((int)c.trial[t].p, 32, 65504) };

				c.bits[t] -= LOG2[y ? p : 65536 - p];
				cm::update(c.trial[t], y);

			}

		}

	};

	struct _set_prior {
		std::vector<u32> start, weight_start, apm_start;
		std::vector<u32> keys, values;
		std::vector<i32> weights;
		std::vector<u16> apm;
		size_t replays{};
	};

	// an apm entry before any update
	u16 flat_apm(size_t j) {
		return (u16)(cm::squash(((int)j - 16) * 128) * 16);
	}

	frames::_prior view(const _set_prior& p) {
		return { p.start.data(), p.keys.data(), p.values.data(), p.weight_start.data(), p.weights.data(), p.apm_start.data(), p.apm.data() };
	}

	_set_prior fit(const std::filesystem::path& cache, u32 set, size_t entries) {

		_set_prior out{};

		// 1. shared models over every replay
		std::vector<_candidate> candidates;

		{
			_set_models M{};
			_counting counting{};

			M.init(set, size_t(1) << 20, true);
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

					_candidate c{ l.model, slot, table.keys[l.index] };

					c.start[0] = (u16)std::clamp<i64>(std::llround((s.ones[at] + 0.5) * 65536.0 / (s.seen[at] + 1.0)), 32, 65503);
					c.start[1] = table.t[at].p;

					candidates.push_back(c);

				}

			}

		}

		// 2. each candidate per replay, from flat and from each start and confidence
		_trials trials{};

		{
			_set_models M{};

			M.init(set, size_t(1) << 20, true);
			trials.models = M.all(set);
			trials.candidates = std::move(candidates);

			for (u32 m{}; m < FRAMES_PRIOR_MODELS; ++m)
				if (trials.models[m])
					trials.row[m].assign(trials.models[m]->table.keys.size(), -1);

			for (size_t i{}; i < trials.candidates.size(); ++i) {

				const auto& c{ trials.candidates[i] };
				const auto& table{ trials.models[c.model]->table };
				i32& r{ trials.row[c.model][c.key & table.mask] };

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

		struct _pick {
			double gain;
			u32 candidate, trial;
		};

		std::vector<_pick> picks;

		for (u32 i{}; i < trials.candidates.size(); ++i) {

			const auto& c{ trials.candidates[i] };
			u32 best{ TRIALS - 1 };

			for (u32 t{}; t < TRIALS - 1; ++t)
				if (c.bits[t] < c.bits[best])
					best = t;

			if (best != TRIALS - 1)
				picks.push_back({ c.bits[TRIALS - 1] - c.bits[best], i, best });

		}

		std::sort(picks.begin(), picks.end(), [](const _pick& a, const _pick& b) {
			return a.gain != b.gain ? a.gain > b.gain : a.candidate < b.candidate;
		});

		if (picks.size() > entries)
			picks.resize(entries);

		// by model, most bits saved first
		std::vector<std::pair<u32, u32>> counters[FRAMES_PRIOR_MODELS];

		for (const auto& p : picks) {

			const auto& c{ trials.candidates[p.candidate] };
			const u32 confidence{ CONFIDENCE[p.trial % CONFIDENCES] };

			counters[c.model].push_back({ (u32)c.key, c.start[p.trial / CONFIDENCES] | confidence << 16 | c.slot << 24 });

		}

		out.start.push_back(0);

		for (u32 m{}; m < FRAMES_PRIOR_MODELS; ++m) {

			for (const auto& [key, value] : counters[m]) {

				out.keys.push_back(key);
				out.values.push_back(value);

			}

			out.start.push_back((u32)out.keys.size());

		}

		out.weight_start.assign(FRAMES_PRIOR_MODELS + 1, 0);
		out.apm_start.assign(FRAMES_PRIOR_MODELS + 1, 0);

		// 3. mixer weights and apm tables, averaged over the replays where they are not at their flat start
		for (int round{}; round < MIX_ROUNDS; ++round) {

			std::vector<double> weights[FRAMES_PRIOR_MODELS], apm[FRAMES_PRIOR_MODELS];
			std::vector<u32> weights_n[FRAMES_PRIOR_MODELS], apm_n[FRAMES_PRIOR_MODELS];
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

					const size_t n{ (size_t)md->inputs }, sets{ md->weights.size() / n }, contexts{ md->refine.t.size() / 33 };

					if (weights[m].empty()) {

						inputs[m] = n;

						weights[m].assign(md->weights.size(), 0);
						weights_n[m].assign(sets, 0);
						apm[m].assign(md->refine.t.size(), 0);
						apm_n[m].assign(contexts, 0);

					}

					for (size_t s{}; s < sets; ++s) {

						const auto w{ md->weights.begin() + s * n };

						if (std::all_of(w, w + n, [](int v) { return v == cm::MIX_WEIGHT; }))
							continue;

						++weights_n[m][s];

						for (size_t i{}; i < n; ++i)
							weights[m][s * n + i] += w[i];

					}

					for (size_t x{}; x < contexts; ++x) {

						bool moved{};

						for (int j{}; j < 33; ++j)
							moved |= md->refine.t[x * 33 + j] != flat_apm(j);

						if (!moved)
							continue;

						++apm_n[m][x];

						for (int j{}; j < 33; ++j)
							apm[m][x * 33 + j] += md->refine.t[x * 33 + j];

					}

				}

			});

			out.weights.clear();
			out.apm.clear();

			for (u32 m{}; m < FRAMES_PRIOR_MODELS; ++m) {

				out.weight_start[m] = (u32)out.weights.size();
				out.apm_start[m] = (u32)out.apm.size();

				for (size_t i{}; i < weights[m].size(); ++i) {

					const u32 n{ weights_n[m][i / inputs[m]] };

					out.weights.push_back(n ? (i32)std::lround(weights[m][i] / n) : cm::MIX_WEIGHT);

				}

				for (size_t i{}; i < apm[m].size(); ++i) {

					const u32 n{ apm_n[m][i / 33] };

					out.apm.push_back(n ? (u16)std::lround(apm[m][i] / n) : flat_apm(i % 33));

				}

			}

			out.weight_start[FRAMES_PRIOR_MODELS] = (u32)out.weights.size();
			out.apm_start[FRAMES_PRIOR_MODELS] = (u32)out.apm.size();

		}

		fprintf(stderr, "set %u: %zu replays, %zu counters\n", set, out.replays, out.keys.size());

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

	void print_starts(const char* name, const _set_prior (&sets)[SETS], std::vector<u32> _set_prior::* starts, const size_t (&base)[SETS]) {

		printf("constexpr uint32_t %s[FRAMES_PRIOR_SETS][FRAMES_PRIOR_MODELS + 1]{\n", name);

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

	if (argc < 2) {

		fprintf(stderr, "usage: frames_prior_gen list.txt [entries] [sets at once] [cache directory] > frames_prior.h\n");

		return 1;
	}

	const size_t entries{ argc > 2 ? (size_t)atoll(argv[2]) : 36000 };
	const u32 at_once{ argc > 3 ? (u32)std::max(1, atoi(argv[3])) : 4u };

	const std::filesystem::path cache{ argc > 4 ? std::filesystem::path(argv[4]) : std::filesystem::temp_directory_path() /
		("frames_prior_gen-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())) };

	std::error_code error;

	std::filesystem::create_directories(cache, error);

	const bool from_stdin{ std::string_view(argv[1]) == "-" };

	std::vector<std::string> paths;

	if (!from_stdin) {

		std::ifstream list(argv[1]);
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

	size_t replays{};

	// encoded once, in parallel a chunk at a time, and kept in their sets' cache files in list order
	{
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

	}

	fprintf(stderr, "encoded %zu replays\n", replays);

	_set_prior sets[SETS];

	{
		std::atomic<u32> next{};
		std::vector<std::thread> workers;

		for (u32 w{}; w < std::min(at_once, SETS); ++w)
			workers.emplace_back([&] {

				for (u32 s; (s = next++) < SETS;)
					sets[s] = fit(cache, s, entries);

			});

		for (auto& w : workers)
			w.join();

	}

	for (u32 s{}; s < SETS; ++s)
		std::filesystem::remove(cache_file(cache, s), error);

	if (argc <= 4)
		std::filesystem::remove(cache, error);

	std::vector<u32> keys, values;
	std::vector<i32> weights;
	std::vector<u16> apm;
	size_t key_base[SETS]{}, weight_base[SETS]{}, apm_base[SETS]{};

	for (u32 s{}; s < SETS; ++s) {

		key_base[s] = keys.size();
		weight_base[s] = weights.size();
		apm_base[s] = apm.size();

		keys.insert(keys.end(), sets[s].keys.begin(), sets[s].keys.end());
		values.insert(values.end(), sets[s].values.begin(), sets[s].values.end());
		weights.insert(weights.end(), sets[s].weights.begin(), sets[s].weights.end());
		apm.insert(apm.end(), sets[s].apm.begin(), sets[s].apm.end());

	}

	printf("#pragma once\n\n#include <cstdint>\n\n");
	printf("// generated by frames_prior_gen.cpp: the starting state of the context mixing models per prior set (frames::prior_set)\n");
	printf("// and model (frames::_models::all, modes::_models::all). Counters are keyed by the low 32 bits of their context hash,\n");
	printf("// value = p | confidence << 16 | slot << 24; then each model's mixer weights and apm table\n\n");
	printf("constexpr uint32_t FRAMES_PRIOR_SETS{ %u };\n", SETS);
	printf("constexpr uint32_t FRAMES_PRIOR_MODELS{ %u };\n\n", FRAMES_PRIOR_MODELS);

	print_starts("FRAMES_PRIOR_START", sets, &_set_prior::start, key_base);
	print_starts("FRAMES_PRIOR_WEIGHT_START", sets, &_set_prior::weight_start, weight_base);
	print_starts("FRAMES_PRIOR_APM_START", sets, &_set_prior::apm_start, apm_base);

	print_array("uint32_t", "FRAMES_PRIOR_KEYS", keys, 8);
	print_array("uint32_t", "FRAMES_PRIOR_VALUES", values, 8);
	print_array("int32_t", "FRAMES_PRIOR_WEIGHTS", weights, 16);
	print_array("uint16_t", "FRAMES_PRIOR_APM", apm, 33);

	return 0;
}
