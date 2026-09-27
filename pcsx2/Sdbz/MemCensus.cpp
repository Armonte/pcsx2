// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Sdbz/MemCensus.h"
#include "Sdbz/RbProfiler.h"

#include "Host.h"
#include "R5900.h"
#include "VMManager.h"

#include "common/Console.h"
#include "common/FileSystem.h"

#include "fmt/format.h"

#include <atomic>
#include <map>
#include <mutex>

namespace MemCensus
{
	namespace
	{
		std::atomic<bool> s_on{false};
		std::vector<Range> s_ranges;
		bool s_reads = true, s_writes = true;
		struct Stat
		{
			u64 count = 0;
			u32 lo = 0xFFFFFFFFu, hi = 0;
			u32 range = 0;
			u64 fields[4] = {0, 0, 0, 0}; // 16-byte buckets of the field offset (up to 4 KiB)
		};
		std::mutex s_mtx;
		std::map<std::pair<u64, u32>, Stat> s_stats; // ((phase << 40) | (store << 32) | pc, $ra = caller of a leaf)

		void ResetRecompiler()
		{
			if (VMManager::HasValidVM())
				Host::RunOnCPUThread([]() { Cpu->Reset(); });
		}
	} // namespace

	void Start(std::vector<Range> ranges, bool reads, bool writes)
	{
		Stop();
		{
			std::lock_guard lk(s_mtx);
			s_ranges = std::move(ranges);
			s_reads = reads;
			s_writes = writes;
			s_stats.clear();
		}
		s_on.store(true);
		ResetRecompiler();
		Console.WriteLn("MemCensus: on (%zu ranges, %s%s)", s_ranges.size(), reads ? "reads " : "", writes ? "writes" : "");
	}

	void Stop()
	{
		if (!s_on.exchange(false))
			return;
		ResetRecompiler();
		Console.WriteLn("MemCensus: off");
	}

	bool Enabled() { return s_on.load(std::memory_order_relaxed); }
	const std::vector<Range>& Ranges() { return s_ranges; }
	bool WantReads() { return s_reads; }
	bool WantWrites() { return s_writes; }

	void Hit(u32 addr, u32 pc_flags)
	{
		const u32 pc = pc_flags & 0x0FFFFFFFu;
		const u32 store = pc_flags >> 31;
		const u32 ph = static_cast<u32>(RbProfiler::CurrentPhase());
		std::lock_guard lk(s_mtx);
		for (u32 i = 0; i < s_ranges.size(); i++)
		{
			const Range& r = s_ranges[i];
			if (addr - r.lo >= r.hi - r.lo)
				continue;
			Stat& s = s_stats[{(static_cast<u64>(ph) << 40) | (static_cast<u64>(store) << 32) | pc, cpuRegs.GPR.n.ra.UL[0]}];
			s.count++;
			s.lo = std::min(s.lo, addr);
			s.hi = std::max(s.hi, addr);
			s.range = i;
			const u32 off = r.stride ? (addr - r.lo) % r.stride : addr - r.lo;
			if (off < 4096)
				s.fields[off >> 10] |= 1ull << ((off >> 4) & 63);
			return;
		}
	}

	std::string Dump()
	{
		static const char* phases[] = {"other", "frame_begin", "capture", "load", "synctest", "resim", "resim_capture", "sim", "render"};
		std::lock_guard lk(s_mtx);
		std::string out = "# MemCensus: pc rw phase count range lo_addr hi_addr fields(field offsets touched, 16-byte buckets) ra\n";
		for (u32 i = 0; i < s_ranges.size(); i++)
			out += fmt::format("# range {}: {:08X}-{:08X} stride {:X}\n", i, s_ranges[i].lo, s_ranges[i].hi, s_ranges[i].stride);
		for (const auto& [kk, s] : s_stats)
		{
			const u64 k = kk.first;
			const u32 pc = static_cast<u32>(k), store = (k >> 32) & 1, ph = static_cast<u32>(k >> 40);
			std::string f;
			for (u32 b = 0; b < 256; b++)
				if (s.fields[b >> 6] & (1ull << (b & 63)))
					f += fmt::format("{}{:X}", f.empty() ? "" : ",", b * 16);
			out += fmt::format("{:08X} {} {} {} {} {:08X} {:08X} {} {:08X}\n", pc, store ? "W" : "R", ph < 9 ? phases[ph] : "?", s.count, s.range,
				s.lo, s.hi, f.empty() ? "-" : f, kk.second);
		}
		return out;
	}

	bool DumpFile(const std::string& path) { return FileSystem::WriteStringToFile(path.c_str(), Dump()); }
} // namespace MemCensus
