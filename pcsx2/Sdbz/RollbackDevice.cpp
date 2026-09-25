// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Sdbz/RollbackDevice.h"
#include "Sdbz/PageSnapshotRing.h"

#include "Memory.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Timer.h"

#include "fmt/format.h"

#include <algorithm>
#include <cstring>
#include <map>
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
		};

		constexpr u32 RAM_MASK = 0x01FFFFFFu;
		constexpr u32 INPUT_HISTORY = 128; // frames of recorded input blocks
		constexpr u32 MAX_RUNS_PER_FRAME = 256;

		std::mutex s_mtx;

		// ---- configuration (Lua) ----
		std::vector<Range> s_cfg_regions, s_cfg_excludes, s_cfg_ignore;
		std::vector<Watch> s_cfg_watches;
		Range s_cfg_input;

		// ---- runtime (EE thread) ----
		Mode s_mode = Mode::Off;
		u32 s_rollback = 0;
		bool s_write_protect = true;
		std::unique_ptr<PageSnapshotRing> s_ring;
		std::vector<Range> s_compare;              // regions - excludes - input - ignore
		std::vector<Watch> s_watches;
		Range s_input;
		std::vector<std::vector<u8>> s_inputs;     // [frame % INPUT_HISTORY]
		std::vector<std::vector<u8>> s_ref;        // sync test: copy of each compare range before rolling back
		s32 s_frame = -1;
		s32 s_first_frame = 0;
		s32 s_resim_base = 0;
		bool s_resim_active = false;
		Common::Timer s_rollback_timer;

		// ---- stats ----
		u64 s_frames = 0, s_rollbacks = 0, s_desync_frames = 0;
		u64 s_last_rollback_us = 0, s_max_rollback_us = 0, s_sum_rollback_us = 0;
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
								s_last_runs.push_back({addr, 1});
						}
						s_page_hits[(r.a + off) >> 12]++;
					}
					off += chunk;
				}
			}
			s_last_diff_bytes = diff_bytes;
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

		void ResetRuntime()
		{
			s_ring.reset();
			s_frame = -1;
			s_resim_active = false;
			s_frames = s_rollbacks = s_desync_frames = 0;
			s_last_rollback_us = s_max_rollback_us = s_sum_rollback_us = 0;
			s_last_diff_bytes = 0;
			s_first_desync_frame = -1;
			s_page_hits.clear();
			s_last_runs.clear();
			s_ref.clear();
		}
	} // namespace

	void ClearConfig()
	{
		std::lock_guard lk(s_mtx);
		s_cfg_regions.clear();
		s_cfg_excludes.clear();
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

	void Start(Mode mode, u32 rollback_frames, bool write_protect)
	{
		std::lock_guard lk(s_mtx);
		ResetRuntime();
		s_mode = mode;
		s_rollback = std::clamp(rollback_frames, 1u, 30u);
		s_write_protect = write_protect;
		s_input = s_cfg_input;
		s_watches = s_cfg_watches;
		s_inputs.assign(INPUT_HISTORY, {});
		std::vector<Range> cut = s_cfg_excludes;
		if (s_input.b > s_input.a)
			cut.push_back(s_input);
		std::vector<Range> cut_cmp = cut;
		cut_cmp.insert(cut_cmp.end(), s_cfg_ignore.begin(), s_cfg_ignore.end());
		s_compare = Subtract(s_cfg_regions, cut_cmp);
		Console.WriteLn("RollbackDevice: mode %d, rollback %u, %zu regions, %zu excludes, compare %zu ranges", static_cast<int>(mode),
			s_rollback, s_cfg_regions.size(), cut.size(), s_compare.size());
	}

	void Stop()
	{
		std::lock_guard lk(s_mtx);
		s_mode = Mode::Off;
		s_ring.reset();
	}

	void OnVMShutdown()
	{
		std::lock_guard lk(s_mtx);
		s_mode = Mode::Off;
		ResetRuntime();
	}

	u64 HandleSyscall(u32 cmd, u32 arg)
	{
		std::lock_guard lk(s_mtx);
		if (s_mode == Mode::Off || !eeMem)
			return 0;

		switch (cmd)
		{
			case CMD_FRAME_BEGIN:
			{
				if (!s_ring)
				{
					std::vector<PageSnapshotRing::Range> regions, excludes;
					for (const Range& r : s_cfg_regions)
						regions.push_back({r.a, r.b - r.a});
					for (const Range& r : s_cfg_excludes)
						excludes.push_back({r.a, r.b - r.a});
					if (s_input.b > s_input.a)
						excludes.push_back({s_input.a, s_input.b - s_input.a});
					s_ring = std::make_unique<PageSnapshotRing>(regions, excludes, s_rollback + 2,
						s_write_protect ? PageSnapshotRing::DirtyMode::WriteProtect : PageSnapshotRing::DirtyMode::Compare);
					s_frame = -1;
					s_first_frame = 0;
				}
				s_frame++;
				s_frames++;
				RecordInput(s_frame);
				s_ring->Capture(s_frame);

				if (s_mode != Mode::SyncTest || s_frame - s_first_frame < static_cast<s32>(s_rollback))
					return 0;

				// Sync test: remember this frame's state, rewind R frames, let the game re-simulate them.
				s_rollback_timer.Reset();
				TakeReference();
				const s32 target = s_frame - static_cast<s32>(s_rollback);
				if (!s_ring->Load(target))
					return 0;
				s_resim_base = target;
				s_resim_active = true;
				s_rollbacks++;
				return s_rollback;
			}

			case CMD_RESIM_PRE:
				if (s_resim_active)
					InjectInput(s_resim_base + static_cast<s32>(arg));
				return 0;

			case CMD_RESIM_POST:
				if (s_resim_active && s_ring)
					s_ring->Capture(s_resim_base + static_cast<s32>(arg) + 1);
				return 0;

			case CMD_CUR_PRE:
				if (s_resim_active)
				{
					if (s_mode == Mode::SyncTest)
						CompareToReference();
					InjectInput(s_frame);
					s_resim_active = false;
					s_last_rollback_us = static_cast<u64>(s_rollback_timer.GetTimeNanoseconds() / 1000.0);
					s_max_rollback_us = std::max(s_max_rollback_us, s_last_rollback_us);
					s_sum_rollback_us += s_last_rollback_us;
				}
				return 0;

			default:
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
									"desync frames {} (first {}) last diff {} B in {} runs",
			s_mode == Mode::SyncTest ? "synctest" : "capture", s_rollback, s_frame, s_rollbacks, s_last_rollback_us, avg,
			s_max_rollback_us, s_desync_frames, s_first_desync_frame, s_last_diff_bytes, s_last_runs.size());
		if (s_ring)
			s += " | " + s_ring->Describe();
		return s;
	}

	bool WriteReport(const std::string& path)
	{
		std::lock_guard lk(s_mtx);
		std::string out = fmt::format("# RollbackDevice sync-test report\nframes {} rollbacks {} desync frames {} first {}\n\n"
									  "## pages that differed after re-simulation (page -> frames)\n",
			s_frames, s_rollbacks, s_desync_frames, s_first_desync_frame);
		std::vector<std::pair<u64, u32>> pages;
		for (const auto& [page, n] : s_page_hits)
			pages.push_back({n, page});
		std::sort(pages.rbegin(), pages.rend());
		for (const auto& [n, page] : pages)
			out += fmt::format("{:08X} {} {}\n", page << 12, n, WatchName(page << 12));
		out += "\n## last frame's differing runs (addr len now/ref first bytes)\n";
		for (const DiffRun& r : s_last_runs)
		{
			std::string now, ref;
			for (u32 k = 0; k < std::min<u32>(r.len, 16); k++)
			{
				now += fmt::format("{:02X}", *Ram(r.addr + k));
				for (size_t i = 0; i < s_compare.size(); i++)
				{
					if (r.addr + k >= s_compare[i].a && r.addr + k < s_compare[i].b && i < s_ref.size())
						ref += fmt::format("{:02X}", s_ref[i][r.addr + k - s_compare[i].a]);
				}
			}
			out += fmt::format("{:08X} {:5} now {} ref {} {}\n", r.addr, r.len, now, ref, WatchName(r.addr));
		}
		return FileSystem::WriteStringToFile(path.c_str(), out);
	}
} // namespace RollbackDevice
