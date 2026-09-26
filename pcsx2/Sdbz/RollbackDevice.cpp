// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Sdbz/RollbackDevice.h"
#include "Sdbz/PageSnapshotRing.h"
#include "Sdbz/RbProfiler.h"
#include "Sdbz/PadFeed.h"

#include "Memory.h"

#include "common/Console.h"
#include "common/Timer.h"

#include "fmt/format.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <tuple>
#include <memory>
#include <mutex>
#include <vector>

namespace RollbackDevice
{
	namespace
	{
		struct Range
		{
			u32 a = 0; // inclusive (EE physical, RAM offset)
			u32 b = 0; // exclusive
		};

		struct Watch
		{
			Range r;
			std::string name;
		};

		struct DiffRun
		{
			u32 addr;
			u32 len;
			u8 now[16]; // re-simulated bytes at compare time
			u8 ref[16]; // pre-rollback bytes
		};

		constexpr u32 RAM_MASK = 0x01FFFFFFu;
		constexpr u32 INPUT_HISTORY = 128; // frames of recorded input blocks
		constexpr u32 MAX_RUNS_PER_FRAME = 256;

		std::mutex s_mtx;

		// ---- configuration (Lua) ----
		std::vector<Range> s_cfg_regions, s_cfg_excludes, s_cfg_ignore;
		std::vector<Range> s_dyn_excludes;         // replaceable exclude set (per-object fields in pools that move)
		bool s_dyn_dirty = false;                  // rebuild the ring at the next FRAME_BEGIN
		u32 s_dyn_rebuilds = 0;
		std::vector<Watch> s_cfg_watches;
		Range s_cfg_input;
		u32 s_cfg_gate = 0;
		std::vector<Range> s_cfg_stable;
		Range s_cfg_rng;

		// ---- runtime (EE thread) ----
		Mode s_mode = Mode::Off;
		u32 s_rollback = 0;
		bool s_write_protect = true;
		std::unique_ptr<PageSnapshotRing> s_ring;
		std::vector<Range> s_compare;              // regions - excludes - input - ignore
		std::vector<Watch> s_watches;
		Range s_input;
		Range s_rng;                               // RNG state split into sim/render streams
		std::vector<u8> s_rng_sim, s_rng_render;   // saved streams while the other one is live
		bool s_in_render = false;
		u32 s_snd_seed = 0x13579BDFu;               // sound RNG stream (host-side; cosmetic, never rolled back)
		u32 s_gate = 0;
		u32 s_gate_prev = 0;
		std::vector<u8> s_gate_ok;                 // [frame % INPUT_HISTORY]: gate counter advanced into this frame
		u64 s_gated_frames = 0;                    // frames where a sync-test rollback was skipped by the gate
		// ---- RNG call tracing ----
		enum class Phase : u8 { Other, NormalSim, Resim, Render };
		bool s_trace = false;
		Phase s_phase = Phase::Other;
		s32 s_phase_frame = 0;
		std::vector<std::vector<u64>> s_norm_trace;  // [frame % INPUT_HISTORY] = (fn << 32 | ra) of the normal sim step
		std::vector<s32> s_norm_trace_frame;
		std::vector<u64> s_cur_trace;
		u64 s_trace_calls_other = 0, s_trace_calls_render = 0, s_trace_mismatches = 0;
		u32 s_trace_other_first_ra = 0;
		std::string s_trace_first_mismatch;
		std::map<u64, u64> s_trace_other_sites;       // (fn<<32|ra) -> count, calls outside sim/render
		std::map<u32, u32> s_trace_alias;             // stub ra -> original call-site ra
		std::map<std::tuple<u32, u32, u32>, u64> s_trace_render_sites; // (fn, task pass, ra) -> count, render section
		u32 s_trace_pass = 0;                         // last task pass reported by the pass marker (trace id 15)

		std::vector<Range> s_stable;
		std::vector<u64> s_stable_hash;            // [frame % INPUT_HISTORY]
		u64 s_io_gated_frames = 0;
		std::vector<std::vector<u8>> s_inputs;     // [frame % INPUT_HISTORY]
		std::vector<std::vector<u8>> s_ref;        // sync test: copy of each compare range before rolling back
		s32 s_frame = -1;
		s32 s_first_frame = 0;
		s32 s_resim_base = 0;
		bool s_resim_active = false;
		Common::Timer s_rollback_timer;

		// ---- stats ----
		u64 s_frames = 0, s_rollbacks = 0, s_desync_frames = 0;
		u64 s_sim_desync_frames = 0;                 // frames where a NAMED WATCH (gameplay state) differed
		s32 s_first_sim_desync_frame = -1;
		std::string s_first_sim_desync;
		u64 s_last_rollback_us = 0, s_max_rollback_us = 0, s_sum_rollback_us = 0;
		// Rollback cost breakdown (sums over all rollbacks, us): sync-test reference copy + compare are test-only
		// overhead; load + resim captures + game re-simulation are the real cost of a netplay rollback.
		u64 s_sum_ref_us = 0, s_sum_load_us = 0, s_sum_cap_us = 0, s_sum_cmp_us = 0;
		Common::Timer s_sim_timer;                 // normal frame: CUR_PRE -> RENDER_BEGIN (sim tick + event pass)
		bool s_sim_timing = false;
		u64 s_sum_sim_us = 0, s_sim_frames = 0;
		// Frame pacing: host time between consecutive FRAME_BEGINs (what the player feels). Histogram in 1 ms bins.
		Common::Timer s_pace_timer;
		bool s_pace_started = false;
		std::array<u32, 101> s_pace_hist{}; // [0..99] ms, [100] = 100 ms+
		u64 s_pace_frames = 0, s_pace_sum_us = 0, s_pace_max_us = 0, s_pace_over = 0, s_pace_over2 = 0;
		u64 s_last_diff_bytes = 0;
		s32 s_first_desync_frame = -1;
		std::map<u32, u64> s_page_hits;             // page -> frames it differed in
		std::vector<DiffRun> s_last_runs;

		std::vector<Range> Subtract(std::vector<Range> base, std::vector<Range> cut)
		{
			std::sort(cut.begin(), cut.end(), [](const Range& x, const Range& y) { return x.a < y.a; });
			std::vector<Range> out;
			for (Range r : base)
			{
				u32 cur = r.a;
				for (const Range& c : cut)
				{
					if (c.b <= cur || c.a >= r.b)
						continue;
					if (c.a > cur)
						out.push_back({cur, c.a});
					cur = std::max(cur, c.b);
					if (cur >= r.b)
						break;
				}
				if (cur < r.b)
					out.push_back({cur, r.b});
			}
			return out;
		}

		u8* Ram(u32 addr) { return &eeMem->Main[addr & RAM_MASK]; }

		void RecordInput(s32 frame)
		{
			if (s_input.b <= s_input.a)
				return;
			auto& slot = s_inputs[static_cast<u32>(frame) % INPUT_HISTORY];
			slot.assign(Ram(s_input.a), Ram(s_input.a) + (s_input.b - s_input.a));
		}

		void InjectInput(s32 frame)
		{
			if (s_input.b <= s_input.a)
				return;
			const auto& slot = s_inputs[static_cast<u32>(frame) % INPUT_HISTORY];
			if (slot.size() == s_input.b - s_input.a)
				std::memcpy(Ram(s_input.a), slot.data(), slot.size());
		}

		std::string WatchName(u32 addr)
		{
			for (const Watch& w : s_watches)
			{
				if (addr >= w.r.a && addr < w.r.b)
					return fmt::format("{}+0x{:X}", w.name, addr - w.r.a);
			}
			return {};
		}

		void TakeReference()
		{
			s_ref.resize(s_compare.size());
			for (size_t i = 0; i < s_compare.size(); i++)
				s_ref[i].assign(Ram(s_compare[i].a), Ram(s_compare[i].a) + (s_compare[i].b - s_compare[i].a));
		}

		// Sync test: the re-simulated state must equal the state captured before the rollback.
		void CompareToReference()
		{
			s_last_runs.clear();
			u64 diff_bytes = 0;
			for (size_t i = 0; i < s_compare.size() && i < s_ref.size(); i++)
			{
				const Range& r = s_compare[i];
				const u8* live = Ram(r.a);
				const u8* ref = s_ref[i].data();
				const u32 len = r.b - r.a;
				if (std::memcmp(live, ref, len) == 0)
					continue;
				u32 off = 0;
				while (off < len)
				{
					const u32 chunk = std::min<u32>(4096 - ((r.a + off) & 4095), len - off);
					if (std::memcmp(live + off, ref + off, chunk) != 0)
					{
						for (u32 j = off; j < off + chunk; j++)
						{
							if (live[j] == ref[j])
								continue;
							diff_bytes++;
							const u32 addr = r.a + j;
							if (!s_last_runs.empty() && addr <= s_last_runs.back().addr + s_last_runs.back().len + 8)
								s_last_runs.back().len = addr + 1 - s_last_runs.back().addr;
							else if (s_last_runs.size() < MAX_RUNS_PER_FRAME)
							{
								DiffRun run{addr, 1, {}, {}};
								const u32 n = std::min<u32>(16, len - j);
								std::memcpy(run.now, live + j, n);
								std::memcpy(run.ref, ref + j, n);
								s_last_runs.push_back(run);
							}
						}
						s_page_hits[(r.a + off) >> 12]++;
					}
					off += chunk;
				}
			}
			s_last_diff_bytes = diff_bytes;
			// Gameplay state (named watches) must never differ; other memory (render caches, leftovers) may.
			std::string sim;
			for (const DiffRun& r : s_last_runs)
			{
				const std::string w = WatchName(r.addr);
				if (!w.empty() && sim.size() < 400)
					sim += fmt::format(" {:08X}+{}({})", r.addr, r.len, w);
			}
			if (!sim.empty())
			{
				s_sim_desync_frames++;
				if (s_first_sim_desync_frame < 0)
				{
					s_first_sim_desync_frame = s_frame;
					s_first_sim_desync = sim;
					Console.Error("RollbackDevice: SIM STATE DESYNC at frame %d:%s", s_frame, sim.c_str());
				}
			}
			if (diff_bytes)
			{
				s_desync_frames++;
				if (s_first_desync_frame < 0)
				{
					s_first_desync_frame = s_frame;
					std::string first;
					for (size_t k = 0; k < std::min<size_t>(s_last_runs.size(), 8); k++)
						first += fmt::format(" {:08X}+{}{}", s_last_runs[k].addr, s_last_runs[k].len,
							WatchName(s_last_runs[k].addr).empty() ? "" : "(" + WatchName(s_last_runs[k].addr) + ")");
					Console.Error("RollbackDevice: SYNC TEST MISMATCH at frame %d: %llu bytes in %zu runs:%s", s_frame,
						diff_bytes, s_last_runs.size(), first.c_str());
				}
			}
		}

		void EndNormalSimTrace()
		{
			if (s_phase == Phase::NormalSim)
			{
				const u32 slot = static_cast<u32>(s_phase_frame) % INPUT_HISTORY;
				s_norm_trace[slot] = s_cur_trace;
				s_norm_trace_frame[slot] = s_phase_frame;
			}
			s_cur_trace.clear();
			s_phase = Phase::Other;
		}

		void CompareResimTrace(s32 frame)
		{
			const u32 slot = static_cast<u32>(frame) % INPUT_HISTORY;
			if (s_norm_trace_frame[slot] != frame)
				return;
			const auto& ref = s_norm_trace[slot];
			const size_t n = std::max(ref.size(), s_cur_trace.size());
			for (size_t i = 0; i < n; i++)
			{
				const u64 a = i < ref.size() ? ref[i] : 0, b = i < s_cur_trace.size() ? s_cur_trace[i] : 0;
				if (a == b)
					continue;
				s_trace_mismatches++;
				if (s_trace_first_mismatch.empty())
				{
					s_trace_first_mismatch = fmt::format("frame {} call #{}: normal fn{} ra {:08X} vs resim fn{} ra {:08X} "
														 "(normal {} calls, resim {})",
						frame, i, a >> 32, static_cast<u32>(a), b >> 32, static_cast<u32>(b), ref.size(), s_cur_trace.size());
					Console.Error("RollbackDevice: RNG TRACE MISMATCH %s", s_trace_first_mismatch.c_str());
				}
				break;
			}
		}

		void ResetRuntime()
		{
			s_ring.reset();
			s_frame = -1;
			s_dyn_rebuilds = 0;
			s_resim_active = false;
			s_frames = s_rollbacks = s_desync_frames = 0;
			s_sim_desync_frames = 0;
			s_first_sim_desync_frame = -1;
			s_first_sim_desync.clear();
			s_last_rollback_us = s_max_rollback_us = s_sum_rollback_us = 0;
			s_sum_ref_us = s_sum_load_us = s_sum_cap_us = s_sum_cmp_us = 0;
			s_sum_sim_us = s_sim_frames = 0;
			s_sim_timing = false;
			s_pace_started = false;
			s_pace_hist.fill(0);
			s_pace_frames = s_pace_sum_us = s_pace_max_us = s_pace_over = s_pace_over2 = 0;
			s_last_diff_bytes = 0;
			s_first_desync_frame = -1;
			s_page_hits.clear();
			s_last_runs.clear();
			s_ref.clear();
		}
	} // namespace

	void SetRngSplit(u32 seed_addr, u32 size)
	{
		std::lock_guard lk(s_mtx);
		s_cfg_rng = {seed_addr & RAM_MASK, (seed_addr & RAM_MASK) + size};
	}

	void SetRngTrace(bool on)
	{
		std::lock_guard lk(s_mtx);
		s_trace = on;
	}

	void AddTraceAlias(u32 ra_from, u32 ra_to)
	{
		std::lock_guard lk(s_mtx);
		s_trace_alias[ra_from] = ra_to;
	}

	void AddGateStable(u32 addr, u32 len)
	{
		std::lock_guard lk(s_mtx);
		s_cfg_stable.push_back({addr & RAM_MASK, (addr & RAM_MASK) + len});
	}

	void SetGate(u32 counter_addr)
	{
		std::lock_guard lk(s_mtx);
		s_cfg_gate = counter_addr & RAM_MASK;
	}

	void ClearConfig()
	{
		std::lock_guard lk(s_mtx);
		s_cfg_gate = 0;
		s_cfg_rng = {};
		s_cfg_stable.clear();
		s_cfg_regions.clear();
		s_cfg_excludes.clear();
		s_dyn_excludes.clear();
		s_cfg_ignore.clear();
		s_cfg_watches.clear();
		s_cfg_input = {};
	}

	void AddRegion(u32 addr, u32 len)
	{
		std::lock_guard lk(s_mtx);
		s_cfg_regions.push_back({addr & RAM_MASK, (addr & RAM_MASK) + len});
	}

	void AddExclude(u32 addr, u32 len)
	{
		std::lock_guard lk(s_mtx);
		s_cfg_excludes.push_back({addr & RAM_MASK, (addr & RAM_MASK) + len});
	}

	void SetInputBlock(u32 addr, u32 len)
	{
		std::lock_guard lk(s_mtx);
		s_cfg_input = {addr & RAM_MASK, (addr & RAM_MASK) + len};
	}

	void AddCompareIgnore(u32 addr, u32 len)
	{
		std::lock_guard lk(s_mtx);
		s_cfg_ignore.push_back({addr & RAM_MASK, (addr & RAM_MASK) + len});
	}

	void AddWatch(u32 addr, u32 len, const std::string& name)
	{
		std::lock_guard lk(s_mtx);
		s_cfg_watches.push_back({{addr & RAM_MASK, (addr & RAM_MASK) + len}, name});
	}

	namespace
	{
		std::vector<Range> AllExcludes()
		{
			std::vector<Range> cut = s_cfg_excludes;
			cut.insert(cut.end(), s_dyn_excludes.begin(), s_dyn_excludes.end());
			if (s_input.b > s_input.a)
				cut.push_back(s_input);
			return cut;
		}

		size_t RebuildCompare()
		{
			std::vector<Range> cut = AllExcludes();
			const size_t n = cut.size();
			cut.insert(cut.end(), s_cfg_ignore.begin(), s_cfg_ignore.end());
			s_compare = Subtract(s_cfg_regions, cut);
			return n;
		}
	} // namespace

	void Start(Mode mode, u32 rollback_frames, bool write_protect)
	{
		std::lock_guard lk(s_mtx);
		ResetRuntime();
		s_mode = mode;
		s_rollback = std::clamp(rollback_frames, 1u, 30u);
		s_write_protect = write_protect;
		s_input = s_cfg_input;
		s_gate = s_cfg_gate;
		s_stable = s_cfg_stable;
		s_phase = Phase::Other;
		s_norm_trace.assign(INPUT_HISTORY, {});
		s_norm_trace_frame.assign(INPUT_HISTORY, -1);
		s_cur_trace.clear();
		s_trace_calls_other = s_trace_calls_render = s_trace_mismatches = 0;
		s_trace_other_first_ra = 0;
		s_trace_first_mismatch.clear();
		s_trace_other_sites.clear();
		s_trace_render_sites.clear();
		s_trace_pass = 0;
		s_stable_hash.assign(INPUT_HISTORY, 0);
		s_io_gated_frames = 0;
		s_rng = s_cfg_rng;
		s_in_render = false;
		s_rng_sim.clear();
		s_rng_render.clear();
		s_gate_ok.assign(INPUT_HISTORY, 0);
		s_gated_frames = 0;
		s_watches = s_cfg_watches;
		s_inputs.assign(INPUT_HISTORY, {});
		s_dyn_dirty = false;
		const size_t n_cut = RebuildCompare();
		Console.WriteLn("RollbackDevice: mode %d, rollback %u, %zu regions, %zu excludes, compare %zu ranges", static_cast<int>(mode),
			s_rollback, s_cfg_regions.size(), n_cut, s_compare.size());
	}

	void SetDynamicExcludes(const std::vector<std::pair<u32, u32>>& ranges)
	{
		std::lock_guard lk(s_mtx);
		std::vector<Range> v;
		v.reserve(ranges.size());
		for (const auto& [addr, len] : ranges)
			v.push_back({addr & RAM_MASK, (addr & RAM_MASK) + len});
		s_dyn_excludes = std::move(v);
		s_dyn_dirty = true; // applied at the next frame boundary (EE thread)
	}

	void Stop()
	{
		std::lock_guard lk(s_mtx);
		s_mode = Mode::Off;
		s_ring.reset();
	}

	namespace
	{
		std::atomic<bool> s_resimulating{false};
		u32 s_bench_every = 10;
	}
	void SetBenchEvery(u32 frames) { s_bench_every = std::max(frames, 1u); }
	bool IsResimulating() { return s_resimulating.load(std::memory_order_relaxed); }

	void OnStateLoaded()
	{
		Mode mode;
		u32 frames;
		bool wp;
		{
			std::lock_guard lk(s_mtx);
			if (s_mode == Mode::Off)
				return;
			mode = s_mode;
			frames = s_rollback;
			wp = s_write_protect;
		}
		Start(mode, frames, wp); // resets runtime state + ring, keeps config
		Console.WriteLn("RollbackDevice: state loaded -> snapshots dropped, restarted (mode %d, R=%u)", static_cast<int>(mode), frames);
	}

	void OnVMShutdown()
	{
		std::lock_guard lk(s_mtx);
		s_mode = Mode::Off;
		ResetRuntime();
	}

	u64 HandleSyscall(u32 cmd, u32 arg, u32 arg2, u32 arg3)
	{
		// Controller feed works whether rollback is running or not (menus, test setup).
		if (cmd == CMD_PAD_FEED)
			return (eeMem && (arg2 & RAM_MASK) < Ps2MemSize::MainRam - 32) ? PadFeed::OnPadRead(arg, Ram(arg2), arg3) : arg3;
		std::lock_guard lk(s_mtx);
		if (s_mode == Mode::Off || !eeMem)
			return 0;

		switch (cmd)
		{
			case CMD_FRAME_BEGIN:
			{
				if (s_dyn_dirty)
				{
					// The dynamic exclude set changed (e.g. a new match allocated new pool blocks): start a new ring.
					// The snapshots before this point are dropped, so the next rollback needs a full window again.
					s_dyn_dirty = false;
					s_ring.reset();
					RebuildCompare();
					s_dyn_rebuilds++;
					Console.WriteLn("RollbackDevice: dynamic excludes changed (%zu ranges): ring rebuilt at frame %d", s_dyn_excludes.size(), s_frame + 1);
				}
				const bool rebuild = !s_ring && s_frame >= 0; // keep frame numbering across a rebuild
				if (!s_ring)
				{
					std::vector<PageSnapshotRing::Range> regions, excludes;
					for (const Range& r : s_cfg_regions)
						regions.push_back({r.a, r.b - r.a});
					for (const Range& r : AllExcludes())
						excludes.push_back({r.a, r.b - r.a});
					s_ring = std::make_unique<PageSnapshotRing>(regions, excludes, s_rollback + 2,
						s_write_protect ? PageSnapshotRing::DirtyMode::WriteProtect : PageSnapshotRing::DirtyMode::Compare);
					if (!rebuild)
					{
						s_frame = -1;
						s_first_frame = 0;
					}
					else
					{
						s_first_frame = s_frame + 1; // no snapshot before this frame
					}
				}
				EndNormalSimTrace(); // (normal sim without a render section this frame)
				RbProfiler::SetPhase(RbProfiler::PH_FRAME_BEGIN);
				if (s_pace_started)
				{
					const u64 us = static_cast<u64>(s_pace_timer.GetTimeNanoseconds() / 1000.0);
					s_pace_hist[std::min<u64>(us / 1000, 100)]++;
					s_pace_frames++;
					s_pace_sum_us += us;
					s_pace_max_us = std::max(s_pace_max_us, us);
					s_pace_over += (us > 16700) ? 1 : 0;   // missed a 60 Hz frame
					s_pace_over2 += (us > 33400) ? 1 : 0;  // missed two
				}
				s_pace_timer.Reset();
				s_pace_started = true;
				s_frame++;
				s_frames++;
				RecordInput(s_frame);
				RbProfiler::SetPhase(RbProfiler::PH_CAPTURE);
				s_ring->Capture(s_frame);
				RbProfiler::SetPhase(RbProfiler::PH_FRAME_BEGIN);

				// Gate: did the live-simulation counter advance into this frame?
				bool ok = true;
				if (s_gate)
				{
					const u32 g = *reinterpret_cast<const u32*>(Ram(s_gate));
					ok = (s_frame > 0) && (g != s_gate_prev);
					s_gate_prev = g;
				}
				s_gate_ok[static_cast<u32>(s_frame) % INPUT_HISTORY] = ok ? 1 : 0;
				{
					u64 h = 1469598103934665603ull; // FNV-1a over the I/O-stable ranges
					for (const Range& r : s_stable)
					{
						const u8* p = Ram(r.a);
						for (u32 k = 0; k < r.b - r.a; k++)
							h = (h ^ p[k]) * 1099511628211ull;
					}
					s_stable_hash[static_cast<u32>(s_frame) % INPUT_HISTORY] = h;
				}

				if ((s_mode != Mode::SyncTest && s_mode != Mode::Bench) || s_frame - s_first_frame < static_cast<s32>(s_rollback))
					return 0;
				if (s_mode == Mode::Bench && (s_frame % static_cast<s32>(s_bench_every)) != 0)
					return 0;
				for (s32 f = s_frame - static_cast<s32>(s_rollback) + 1; f <= s_frame; f++)
				{
					if (!s_gate_ok[static_cast<u32>(f) % INPUT_HISTORY])
					{
						s_gated_frames++;
						return 0; // window touches a non-gameplay frame: never rewind across it
					}
				}
				if (!s_stable.empty())
				{
					const u64 h0 = s_stable_hash[static_cast<u32>(s_frame - static_cast<s32>(s_rollback)) % INPUT_HISTORY];
					for (s32 f = s_frame - static_cast<s32>(s_rollback) + 1; f <= s_frame; f++)
					{
						if (s_stable_hash[static_cast<u32>(f) % INPUT_HISTORY] != h0)
						{
							s_io_gated_frames++;
							return 0; // async I/O was submitted inside the window: never re-issue it
						}
					}
				}

				// Sync test: remember this frame's state, rewind R frames, let the game re-simulate them.
				s_rollback_timer.Reset();
				Common::Timer t;
				RbProfiler::SetPhase(RbProfiler::PH_SYNCTEST);
				if (s_mode == Mode::SyncTest)
					TakeReference();
				s_sum_ref_us += static_cast<u64>(t.GetTimeNanoseconds() / 1000.0);
				const s32 target = s_frame - static_cast<s32>(s_rollback);
				t.Reset();
				RbProfiler::SetPhase(RbProfiler::PH_LOAD);
				if (!s_ring->Load(target))
					return 0;
				s_sum_load_us += static_cast<u64>(t.GetTimeNanoseconds() / 1000.0);
				s_resim_base = target;
				s_resim_active = true;
				s_resimulating.store(true, std::memory_order_relaxed);
				s_rollbacks++;
				return s_rollback;
			}

			case CMD_RESIM_PRE:
				RbProfiler::SetPhase(RbProfiler::PH_RESIM);
				if (s_resim_active)
					InjectInput(s_resim_base + static_cast<s32>(arg));
				s_phase = Phase::Resim;
				s_phase_frame = s_resim_base + static_cast<s32>(arg);
				s_cur_trace.clear();
				return 0;

			case CMD_RESIM_POST:
				if (s_trace && s_phase == Phase::Resim)
					CompareResimTrace(s_phase_frame);
				s_phase = Phase::Other;
				s_cur_trace.clear();
				if (s_resim_active && s_ring)
				{
					Common::Timer t;
					RbProfiler::SetPhase(RbProfiler::PH_RESIM_CAPTURE);
					s_ring->Capture(s_resim_base + static_cast<s32>(arg) + 1);
					s_sum_cap_us += static_cast<u64>(t.GetTimeNanoseconds() / 1000.0);
				}
				return 0;

			// Sound RNG stream: draws whose result only picks volume/pan/pitch/voice variants. Keeping them off the
			// simulation stream makes the simulation independent of audio state (sound is never rolled back and can
			// differ between netplay peers, e.g. "play only if the voice channel is free").
			case CMD_SND_RAND_INT:
			{
				s_snd_seed = s_snd_seed * 214013u + 2531011u;
				const s32 lo = static_cast<s32>(arg), hi = static_cast<s32>(arg2);
				return static_cast<u64>(static_cast<s64>(lo + static_cast<s32>((static_cast<s64>(s_snd_seed >> 16) * (hi - lo)) >> 16)));
			}
			case CMD_SND_RAND_FLOAT:
			{
				s_snd_seed = s_snd_seed * 214013u + 2531011u;
				float lo, hi;
				std::memcpy(&lo, &arg, 4);
				std::memcpy(&hi, &arg2, 4);
				const float r = lo + (hi - lo) * (static_cast<float>(s_snd_seed >> 16) / 65536.0f);
				u32 bits;
				std::memcpy(&bits, &r, 4);
				return bits;
			}

			case CMD_RENDER_BEGIN:
				if (s_sim_timing)
				{
					s_sum_sim_us += static_cast<u64>(s_sim_timer.GetTimeNanoseconds() / 1000.0);
					s_sim_frames++;
					s_sim_timing = false;
				}
				EndNormalSimTrace();
				s_phase = Phase::Render;
				RbProfiler::SetPhase(RbProfiler::PH_RENDER);
				if (s_rng.b > s_rng.a && !s_in_render)
				{
					const u32 n = s_rng.b - s_rng.a;
					s_rng_sim.assign(Ram(s_rng.a), Ram(s_rng.a) + n);
					if (s_rng_render.size() != n)
					{
						// first frame: derive a render stream from the sim stream (any value works; it is cosmetic)
						s_rng_render = s_rng_sim;
						for (u8& b : s_rng_render)
							b ^= 0xA5;
					}
					std::memcpy(Ram(s_rng.a), s_rng_render.data(), n);
					s_in_render = true;
				}
				return 0;

			case CMD_RENDER_END:
				s_phase = Phase::Other;
				RbProfiler::SetPhase(RbProfiler::PH_OTHER);
				if (s_in_render)
				{
					const u32 n = s_rng.b - s_rng.a;
					s_rng_render.assign(Ram(s_rng.a), Ram(s_rng.a) + n);
					std::memcpy(Ram(s_rng.a), s_rng_sim.data(), n);
					s_in_render = false;
				}
				return 0;

			case CMD_CUR_PRE:
				if (s_resim_active)
				{
					if (s_mode == Mode::SyncTest)
					{
						Common::Timer t;
						RbProfiler::SetPhase(RbProfiler::PH_SYNCTEST);
						CompareToReference();
						s_sum_cmp_us += static_cast<u64>(t.GetTimeNanoseconds() / 1000.0);
					}
					InjectInput(s_frame);
					s_resim_active = false;
					s_resimulating.store(false, std::memory_order_relaxed);
					s_last_rollback_us = static_cast<u64>(s_rollback_timer.GetTimeNanoseconds() / 1000.0);
					s_max_rollback_us = std::max(s_max_rollback_us, s_last_rollback_us);
					s_sum_rollback_us += s_last_rollback_us;
				}
				s_phase = Phase::NormalSim;
				RbProfiler::SetPhase(RbProfiler::PH_SIM);
				s_phase_frame = s_frame;
				s_sim_timing = s_gate_ok[static_cast<u32>(s_frame) % INPUT_HISTORY] != 0; // gameplay frames only
				s_sim_timer.Reset();
				s_cur_trace.clear();
				return 0;

			default:
				if (cmd >= CMD_RNG_TRACE && cmd < CMD_RNG_TRACE + 16 && s_trace)
				{
					if (cmd == CMD_RNG_TRACE + TRACE_PASS_MARKER)
					{
						s_trace_pass = arg2; // Task_RunList(list, pass): attributes the following calls to a task pass
						return 0;
					}
					const auto al = s_trace_alias.find(arg);
					const u32 ra = (al != s_trace_alias.end()) ? al->second : arg;
					const u64 key = (static_cast<u64>(cmd - CMD_RNG_TRACE) << 32) | ra;
					switch (s_phase)
					{
						case Phase::NormalSim:
						case Phase::Resim:
							if (cmd - CMD_RNG_TRACE < TRACE_PROBE_FIRST) // probes are a render-section census only
								s_cur_trace.push_back(key);
							break;
						case Phase::Render:
							s_trace_calls_render++;
							s_trace_render_sites[{cmd - CMD_RNG_TRACE, s_trace_pass, ra}]++;
							break;
						default:
							s_trace_calls_other++;
							if (!s_trace_other_first_ra)
								s_trace_other_first_ra = arg;
							s_trace_other_sites[key]++;
							break;
					}
				}
				return 0;
		}
	}

	std::string Status()
	{
		std::lock_guard lk(s_mtx);
		if (s_mode == Mode::Off)
			return "rbdev: off";
		const u64 avg = s_rollbacks ? s_sum_rollback_us / s_rollbacks : 0;
		std::string s = fmt::format("rbdev: {} R={} frame {} | rollbacks {} (last {} us, avg {} us, max {} us) | "
									"SIM DESYNC frames {} (first {}) | other-memory diff frames {} (last {} B in {} runs)",
			s_mode == Mode::SyncTest ? "synctest" : s_mode == Mode::Bench ? "bench" : "capture", s_rollback, s_frame, s_rollbacks, s_last_rollback_us, avg,
			s_max_rollback_us, s_sim_desync_frames, s_first_sim_desync_frame, s_desync_frames, s_last_diff_bytes,
			s_last_runs.size());
		if (s_rollbacks)
		{
			const u64 n = s_rollbacks, other = s_sum_ref_us + s_sum_load_us + s_sum_cap_us + s_sum_cmp_us;
			s += fmt::format(" | normal sim step {} us/frame", s_sim_frames ? s_sum_sim_us / s_sim_frames : 0);
			s += fmt::format(" | avg per rollback: load {} us, resim game {} us, resim captures {} us (real {} us) + test-only ref {} us, compare {} us",
				s_sum_load_us / n, (s_sum_rollback_us > other ? s_sum_rollback_us - other : 0) / n, s_sum_cap_us / n,
				(s_sum_rollback_us > s_sum_ref_us + s_sum_cmp_us ? s_sum_rollback_us - s_sum_ref_us - s_sum_cmp_us : 0) / n,
				s_sum_ref_us / n, s_sum_cmp_us / n);
		}
		if (s_pace_frames)
		{
			u64 acc = 0, p99 = 0;
			for (u32 b = 0; b <= 100; b++)
			{
				acc += s_pace_hist[b];
				if (acc * 100 >= s_pace_frames * 99) { p99 = b; break; }
			}
			s += fmt::format(" | frame pacing: avg {:.2f} ms, p99 {} ms, max {:.1f} ms, >16.7 ms {} ({:.1f}%), >33.4 ms {}",
				s_pace_sum_us / 1000.0 / s_pace_frames, p99, s_pace_max_us / 1000.0, s_pace_over,
				100.0 * s_pace_over / s_pace_frames, s_pace_over2);
		}
		s += fmt::format(" | gated (no rollback) frames {} (+{} for I/O) | ring rebuilds {} ({} dynamic excludes)", s_gated_frames,
			s_io_gated_frames, s_dyn_rebuilds, s_dyn_excludes.size());
		if (s_trace)
			s += fmt::format(" | RNG trace: mismatches {} first [{}] | calls outside sim/render {} (first ra {:08X}), in render {}",
				s_trace_mismatches, s_trace_first_mismatch, s_trace_calls_other, s_trace_other_first_ra, s_trace_calls_render);
		if (s_ring)
			s += " | " + s_ring->Describe();
		return s;
	}

	std::string ReportText()
	{
		std::lock_guard lk(s_mtx);
		std::string out = fmt::format("# RollbackDevice sync-test report\nframes {} rollbacks {} | SIM (watched) desync frames {} "
									  "first {}:{}\nother-memory diff frames {} first {}\n\n"
									  "## pages that differed after re-simulation (page -> frames)\n",
			s_frames, s_rollbacks, s_sim_desync_frames, s_first_sim_desync_frame, s_first_sim_desync, s_desync_frames,
			s_first_desync_frame);
		std::vector<std::pair<u64, u32>> pages;
		for (const auto& [page, n] : s_page_hits)
			pages.push_back({n, page});
		std::sort(pages.rbegin(), pages.rend());
		for (const auto& [n, page] : pages)
			out += fmt::format("{:08X} {} {}\n", page << 12, n, WatchName(page << 12));
		out += "\n## RNG calls outside the sim step and the render section ((fn, caller) -> count)\n";
		for (const auto& [key, n] : s_trace_other_sites)
			out += fmt::format("fn{} ra {:08X}  {}\n", key >> 32, static_cast<u32>(key), n);
		out += "\n## traced calls in the render section ((fn, task pass, caller) -> count)\n";
		for (const auto& [key, n] : s_trace_render_sites)
			out += fmt::format("fn{} pass {:X} ra {:08X}  {}\n", std::get<0>(key), std::get<1>(key), std::get<2>(key), n);
		out += fmt::format("\n## RNG trace first mismatch\n{}\n", s_trace_first_mismatch);
		out += "\n## last frame's differing runs (addr len now/ref first bytes)\n";
		for (const DiffRun& r : s_last_runs)
		{
			std::string now, ref;
			for (u32 k = 0; k < std::min<u32>(r.len, 16); k++)
			{
				now += fmt::format("{:02X}", r.now[k]);
				ref += fmt::format("{:02X}", r.ref[k]);
			}
			out += fmt::format("{:08X} {:5} now {} ref {} {}\n", r.addr, r.len, now, ref, WatchName(r.addr));
		}
		return out;
	}
} // namespace RollbackDevice
