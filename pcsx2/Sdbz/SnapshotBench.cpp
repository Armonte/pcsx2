// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Sdbz/SnapshotBench.h"
#include "Sdbz/PageSnapshotRing.h"

#include "Memory.h"

#include "common/Console.h"

#include "fmt/format.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

namespace SnapshotBench
{
	namespace
	{
		enum Cmd : int
		{
			CmdNone = 0,
			CmdStart,
			CmdStop,
			CmdRollback,
		};

		std::mutex s_mtx;
		std::atomic<int> s_cmd{CmdNone};

		std::vector<PageSnapshotRing::Range> s_regions;
		std::vector<PageSnapshotRing::Range> s_excludes;
		u32 s_capacity = 7; // Slippi ROLLBACK_MAX_FRAMES
		bool s_write_protect = false;
		u32 s_rollback_frames = 0;
		bool s_verify = false;

		std::unique_ptr<PageSnapshotRing> s_ring;
		s32 s_frame = 0;

		// Worst-case stats since Start() (the per-frame budget is what matters for rollback).
		u64 s_max_capture_us = 0;
		u64 s_max_load_us = 0;
		u32 s_max_dirty_pages = 0;
		u64 s_sum_dirty_pages = 0;
		u32 s_captures = 0;
		u32 s_verify_failures = 0;
		u32 s_verify_bad_pages = 0;
		u32 s_first_bad_addr = 0;

		bool InRange(const std::vector<PageSnapshotRing::Range>& ranges, u32 addr)
		{
			return std::any_of(ranges.begin(), ranges.end(),
				[addr](const PageSnapshotRing::Range& r) { return addr >= r.address && addr < r.address + r.length; });
		}

		// Byte-compare RAM against a freshly taken reference copy of the tracked regions.
		// Used after Load(): RAM must equal the reference taken at capture time outside excludes.
		struct RegionCopy
		{
			std::vector<std::vector<u8>> data;
		};

		std::vector<RegionCopy> s_refs; // one full reference copy per ring slot (verify mode only)
		std::vector<s32> s_ref_frames;

		void TakeRef(s32 frame)
		{
			RegionCopy rc;
			for (const auto& r : s_regions)
			{
				std::vector<u8> d(r.length);
				std::memcpy(d.data(), &eeMem->Main[r.address & 0x01FFFFFFu], r.length);
				rc.data.push_back(std::move(d));
			}
			s_refs.push_back(std::move(rc));
			s_ref_frames.push_back(frame);
			while (s_refs.size() > s_capacity)
			{
				s_refs.erase(s_refs.begin());
				s_ref_frames.erase(s_ref_frames.begin());
			}
		}

		void VerifyAgainstRef(s32 frame, const std::vector<PageSnapshotRing::Range>& skip)
		{
			auto it = std::find(s_ref_frames.begin(), s_ref_frames.end(), frame);
			if (it == s_ref_frames.end())
				return;
			const RegionCopy& rc = s_refs[it - s_ref_frames.begin()];
			u32 bad = 0;
			for (size_t ri = 0; ri < s_regions.size(); ri++)
			{
				const auto& r = s_regions[ri];
				const u8* live = &eeMem->Main[r.address & 0x01FFFFFFu];
				for (u32 off = 0; off < r.length; off += 4096)
				{
					const u32 n = std::min<u32>(4096, r.length - off);
					if (std::memcmp(live + off, rc.data[ri].data() + off, n) == 0)
						continue;
					for (u32 b = 0; b < n; b++)
					{
						const u32 addr = r.address + off + b;
						if (live[off + b] != rc.data[ri][off + b] && !InRange(skip, addr))
						{
							if (!bad && !s_verify_bad_pages)
								s_first_bad_addr = addr;
							bad++;
							break;
						}
					}
				}
			}
			if (bad)
			{
				s_verify_failures++;
				s_verify_bad_pages += bad;
				Console.Error("SnapshotBench: VERIFY FAILED after load of frame %d: %u page(s) differ (first 0x%08X)",
					frame, bad, s_first_bad_addr);
			}
		}

		void ResetStats()
		{
			s_max_capture_us = s_max_load_us = 0;
			s_max_dirty_pages = 0;
			s_sum_dirty_pages = 0;
			s_captures = 0;
			s_verify_failures = s_verify_bad_pages = 0;
			s_first_bad_addr = 0;
			s_refs.clear();
			s_ref_frames.clear();
		}
	} // namespace

	void ClearRegions()
	{
		std::lock_guard lk(s_mtx);
		s_regions.clear();
		s_excludes.clear();
	}

	void AddRegion(u32 addr, u32 len)
	{
		std::lock_guard lk(s_mtx);
		s_regions.push_back({addr, len});
	}

	void AddExclude(u32 addr, u32 len)
	{
		std::lock_guard lk(s_mtx);
		s_excludes.push_back({addr, len});
	}

	void Start(u32 capacity, bool write_protect)
	{
		std::lock_guard lk(s_mtx);
		s_capacity = std::clamp(capacity, 1u, 120u);
		s_write_protect = write_protect;
		s_cmd.store(CmdStart);
	}

	void Stop()
	{
		s_cmd.store(CmdStop);
	}

	void Rollback(u32 frames_back)
	{
		std::lock_guard lk(s_mtx);
		s_rollback_frames = frames_back;
		s_cmd.store(CmdRollback);
	}

	void SetVerify(bool enabled)
	{
		std::lock_guard lk(s_mtx);
		s_verify = enabled;
	}

	void OnVMShutdown()
	{
		std::lock_guard lk(s_mtx);
		s_ring.reset();
		s_refs.clear();
		s_ref_frames.clear();
	}

	void OnVSyncStart()
	{
		if (!eeMem)
			return;

		std::lock_guard lk(s_mtx);

		switch (s_cmd.exchange(CmdNone))
		{
			case CmdStart:
				s_ring.reset(); // disables write tracking of a previous ring first
				if (s_regions.empty())
				{
					Console.Error("SnapshotBench: no regions configured (snap.add_region)");
					break;
				}
				ResetStats();
				s_frame = 0;
				s_ring = std::make_unique<PageSnapshotRing>(s_regions, s_excludes, s_capacity,
					s_write_protect ? PageSnapshotRing::DirtyMode::WriteProtect : PageSnapshotRing::DirtyMode::Compare);
				break;

			case CmdStop:
				s_ring.reset();
				Console.WriteLn("SnapshotBench: stopped");
				break;

			case CmdRollback:
				if (s_ring)
				{
					const s32 target = s_frame - 1 - static_cast<s32>(s_rollback_frames);
					if (!s_ring->Load(target))
					{
						Console.Error("SnapshotBench: frame %d not in the ring", target);
						break;
					}
					s_max_load_us = std::max(s_max_load_us, s_ring->GetStats().last_load_us);
					if (s_verify)
						VerifyAgainstRef(target, s_excludes);
					// Drop references newer than the target, continue numbering from it.
					while (!s_ref_frames.empty() && s_ref_frames.back() > target)
					{
						s_ref_frames.pop_back();
						s_refs.pop_back();
					}
					s_frame = target + 1;
					Console.WriteLn("SnapshotBench: rolled back to frame %d: %s", target, s_ring->Describe().c_str());
					return; // memory is the target frame; the next VSync captures target+1
				}
				break;

			default:
				break;
		}

		if (!s_ring)
			return;

		s_ring->Capture(s_frame);
		const auto& st = s_ring->GetStats();
		s_max_capture_us = std::max(s_max_capture_us, st.last_capture_us);
		s_max_dirty_pages = std::max(s_max_dirty_pages, st.last_dirty_pages);
		if (s_captures > 0) // the first capture is the full copy
			s_sum_dirty_pages += st.last_dirty_pages;
		s_captures++;
		if (s_verify)
			TakeRef(s_frame);
		s_frame++;
	}

	std::string Status()
	{
		std::lock_guard lk(s_mtx);
		if (!s_ring)
			return "snap: off";
		const u32 incr = s_captures > 1 ? s_captures - 1 : 1;
		return fmt::format("snap: frame {} | {} | max capture {} us, max load {} us | dirty pages avg {:.1f} max {} | "
						   "verify {} ({} fail, {} bad pages, first 0x{:08X})",
			s_frame, s_ring->Describe(), s_max_capture_us, s_max_load_us,
			static_cast<double>(s_sum_dirty_pages) / incr, s_max_dirty_pages, s_verify ? "on" : "off", s_verify_failures,
			s_verify_bad_pages, s_first_bad_addr);
	}
} // namespace SnapshotBench
