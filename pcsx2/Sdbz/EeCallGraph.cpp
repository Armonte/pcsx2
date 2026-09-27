// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Sdbz/EeCallGraph.h"
#include "Sdbz/RollbackDevice.h"

#include "Host.h"
#include "R5900.h"
#include "VMManager.h"

#include "common/Console.h"
#include "common/FileSystem.h"

#include "fmt/format.h"

#include <atomic>
#include <map>
#include <mutex>
#include <unordered_map>

namespace EeCallGraph
{
	namespace
	{
		std::atomic<bool> s_on{false};
		struct Frame
		{
			u32 fn, ret;
			u64 cycle, child;
		};
		constexpr u32 MAX_DEPTH = 256;
		Frame s_stack[MAX_DEPTH];
		std::atomic<u32> s_depth{0};
		u32 s_overflow = 0; // pushes beyond MAX_DEPTH (not tracked)
		struct Stat
		{
			u64 calls[2] = {0, 0}, incl[2] = {0, 0}, self[2] = {0, 0}, host_incl[2] = {0, 0}, host_self[2] = {0, 0};
		};
		std::mutex s_mtx; // stats (EE thread writes, sampler writes host_*, dump reads)
		std::unordered_map<u32, Stat> s_stats;
		u64 s_unmatched = 0, s_unwound = 0;

		void ResetRecompiler()
		{
			if (VMManager::HasValidVM())
				Host::RunOnCPUThread([]() { Cpu->Reset(); });
		}
	} // namespace

	void Start()
	{
		{
			std::lock_guard lk(s_mtx);
			s_stats.clear();
			s_unmatched = s_unwound = 0;
		}
		s_depth = 0;
		s_overflow = 0;
		s_on.store(true);
		ResetRecompiler();
		Console.WriteLn("EeCallGraph: on (recompiler flushed; shadow call stack on every jal/jalr/jr ra)");
	}

	void Stop()
	{
		if (!s_on.exchange(false))
			return;
		ResetRecompiler();
		Console.WriteLn("EeCallGraph: off");
	}

	bool Enabled() { return s_on.load(std::memory_order_relaxed); }

	void Push(u32 target, u32 ret)
	{
		const u32 d = s_depth.load(std::memory_order_relaxed);
		if (d >= MAX_DEPTH)
		{
			s_overflow++;
			return;
		}
		s_stack[d] = {target, ret, cpuRegs.cycle, 0};
		s_depth.store(d + 1, std::memory_order_release);
	}

	void Pop(u32 target)
	{
		u32 d = s_depth.load(std::memory_order_relaxed);
		// the frame this return belongs to: the topmost one whose return address is the jr target
		u32 k = d;
		for (u32 i = d; i > 0 && d - i < 32; i--)
			if (s_stack[i - 1].ret == target)
			{
				k = i - 1;
				break;
			}
		if (k == d)
		{
			s_unmatched++;
			return;
		}
		const int r = RollbackDevice::IsResimulating() ? 1 : 0;
		const u64 now = cpuRegs.cycle;
		std::lock_guard lk(s_mtx);
		while (d > k)
		{
			const Frame& f = s_stack[--d];
			const u64 incl = now - f.cycle;
			Stat& s = s_stats[f.fn];
			s.calls[r]++;
			s.incl[r] += incl;
			s.self[r] += incl - std::min(incl, f.child);
			if (d > k)
				s_unwound++;
			if (d > 0)
				s_stack[d - 1].child += incl;
		}
		s_depth.store(d, std::memory_order_release);
	}

	void HostSample(bool resim)
	{
		const u32 d = std::min(s_depth.load(std::memory_order_acquire), MAX_DEPTH);
		if (!d)
			return;
		const int r = resim ? 1 : 0;
		u32 seen[MAX_DEPTH];
		u32 n = 0;
		std::lock_guard lk(s_mtx);
		for (u32 i = 0; i < d; i++)
		{
			const u32 fn = s_stack[i].fn;
			bool dup = false;
			for (u32 j = 0; j < n && !dup; j++)
				dup = seen[j] == fn;
			if (dup)
				continue; // recursion: count a function once per sample
			seen[n++] = fn;
			s_stats[fn].host_incl[r]++;
		}
		s_stats[s_stack[d - 1].fn].host_self[r]++;
	}

	std::string Dump()
	{
		std::lock_guard lk(s_mtx);
		std::string out = fmt::format("# EeCallGraph: fn calls_resim calls_normal incl_resim self_resim incl_normal self_normal host_incl_resim "
									  "host_self_resim host_incl_normal host_self_normal (unmatched returns {}, unwound frames {}, overflow {})\n",
			s_unmatched, s_unwound, s_overflow);
		std::map<u32, const Stat*> sorted;
		for (const auto& [fn, s] : s_stats)
			sorted[fn] = &s;
		for (const auto& [fn, s] : sorted)
			out += fmt::format("{:08X} {} {} {} {} {} {} {} {} {} {}\n", fn, s->calls[1], s->calls[0], s->incl[1], s->self[1], s->incl[0], s->self[0],
				s->host_incl[1], s->host_self[1], s->host_incl[0], s->host_self[0]);
		return out;
	}

	bool DumpFile(const std::string& path) { return FileSystem::WriteStringToFile(path.c_str(), Dump()); }
} // namespace EeCallGraph
