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
#include <vector>

namespace EeCallGraph
{
	namespace
	{
		std::atomic<bool> s_on{false};
		struct Frame
		{
			u32 fn, ret;
			u64 cycle, child;
			u32 node; // calling-context-tree node of this frame
		};
		// Calling-context tree: one node per distinct call path (parent node, callee); node 0 = root
		struct CctNode
		{
			u32 parent, fn;
			u64 calls[2], incl[2], self[2];
		};
		constexpr u32 MAX_NODES = 1u << 20;
		std::vector<CctNode> s_cct;                   // EE thread (+ dump under s_mtx)
		std::unordered_map<u64, u32> s_cct_index;     // (parent << 32) | fn -> node
		std::vector<u64> s_cct_host[2];               // host samples per node (sampler, s_host_mtx), inclusive via parents
		u32 CctChild(u32 parent, u32 fn)
		{
			const u64 k = (static_cast<u64>(parent) << 32) | fn;
			if (const auto it = s_cct_index.find(k); it != s_cct_index.end())
				return it->second;
			if (s_cct.size() >= MAX_NODES)
				return parent; // full: attribute to the parent context
			const u32 id = static_cast<u32>(s_cct.size());
			s_cct.push_back({parent, fn, {0, 0}, {0, 0}, {0, 0}});
			s_cct_index.emplace(k, id);
			return id;
		}
		constexpr u32 MAX_DEPTH = 256;
		Frame s_stack[MAX_DEPTH];
		std::atomic<u32> s_depth{0};
		u32 s_overflow = 0; // pushes beyond MAX_DEPTH (not tracked)
		struct Stat
		{
			u64 calls[2] = {0, 0}, incl[2] = {0, 0}, self[2] = {0, 0}, host_incl[2] = {0, 0}, host_self[2] = {0, 0};
		};
		std::mutex s_mtx; // s_stats: EE thread + dump only. The sampler NEVER takes it: it suspends the EE thread,
		                  // which may hold it (deadlock) -- host samples go to s_host under s_host_mtx instead.
		std::unordered_map<u32, Stat> s_stats;
		struct EdgeStat
		{
			u64 calls[2] = {0, 0}, incl[2] = {0, 0};
		};
		std::unordered_map<u64, EdgeStat> s_edges; // (caller << 32) | callee
		struct HostStat
		{
			u64 incl[2] = {0, 0}, self[2] = {0, 0};
		};
		std::mutex s_host_mtx;
		std::unordered_map<u32, HostStat> s_host;
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
			s_edges.clear();
			s_cct.clear();
			s_cct.reserve(1u << 16);
			s_cct.push_back({0, 0, {0, 0}, {0, 0}, {0, 0}});
			s_cct_index.clear();
			s_unmatched = s_unwound = 0;
		}
		{
			std::lock_guard lk(s_host_mtx);
			s_host.clear();
			s_cct_host[0].assign(MAX_NODES, 0);
			s_cct_host[1].assign(MAX_NODES, 0);
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
		u32 node;
		{
			std::lock_guard lk(s_mtx);
			node = CctChild(d ? s_stack[d - 1].node : 0, target);
		}
		s_stack[d] = {target, ret, cpuRegs.cycle, 0, node};
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
			if (f.node < s_cct.size())
			{
				CctNode& n = s_cct[f.node];
				n.calls[r]++;
				n.incl[r] += incl;
				n.self[r] += incl - std::min(incl, f.child);
			}
			if (d > k)
				s_unwound++;
			if (d > 0)
			{
				s_stack[d - 1].child += incl;
				EdgeStat& e = s_edges[(static_cast<u64>(s_stack[d - 1].fn) << 32) | f.fn];
				e.calls[r]++;
				e.incl[r] += incl;
			}
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
		// the EE thread is suspended: never block on a lock it might hold (Dump runs on it) -- drop the sample instead
		std::unique_lock lk(s_host_mtx, std::try_to_lock);
		if (!lk.owns_lock())
			return;
		for (u32 i = 0; i < d; i++)
		{
			const u32 fn = s_stack[i].fn;
			bool dup = false;
			for (u32 j = 0; j < n && !dup; j++)
				dup = seen[j] == fn;
			if (dup)
				continue; // recursion: count a function once per sample
			seen[n++] = fn;
			s_host[fn].incl[r]++;
		}
		s_host[s_stack[d - 1].fn].self[r]++;
		if (s_cct_host[r].size() == MAX_NODES && s_stack[d - 1].node < MAX_NODES)
			s_cct_host[r][s_stack[d - 1].node]++; // self; the dump sums it up the parent chain
	}

	std::string Dump()
	{
		std::lock_guard lk(s_mtx);
		std::lock_guard lk2(s_host_mtx);
		for (const auto& [fn, h] : s_host)
			for (int r = 0; r < 2; r++)
			{
				s_stats[fn].host_incl[r] = h.incl[r];
				s_stats[fn].host_self[r] = h.self[r];
			}
		std::string out = fmt::format("# EeCallGraph: fn calls_resim calls_normal incl_resim self_resim incl_normal self_normal host_incl_resim "
									  "host_self_resim host_incl_normal host_self_normal (unmatched returns {}, unwound frames {}, overflow {})\n",
			s_unmatched, s_unwound, s_overflow);
		std::map<u32, const Stat*> sorted;
		for (const auto& [fn, s] : s_stats)
			sorted[fn] = &s;
		for (const auto& [fn, s] : sorted)
			out += fmt::format("{:08X} {} {} {} {} {} {} {} {} {} {}\n", fn, s->calls[1], s->calls[0], s->incl[1], s->self[1], s->incl[0], s->self[0],
				s->host_incl[1], s->host_self[1], s->host_incl[0], s->host_self[0]);
		// calling-context tree: N id parent fn calls_r incl_r self_r calls_n incl_n self_n hostself_r hostself_n
		out += "# cct: N id parent fn calls_resim incl_resim self_resim calls_normal incl_normal self_normal host_self_resim host_self_normal\n";
		for (u32 i = 1; i < s_cct.size(); i++)
		{
			const CctNode& n = s_cct[i];
			const u64 h1 = i < s_cct_host[1].size() ? s_cct_host[1][i] : 0, h0 = i < s_cct_host[0].size() ? s_cct_host[0][i] : 0;
			out += fmt::format("N {} {} {:08X} {} {} {} {} {} {} {} {}\n", i, n.parent, n.fn, n.calls[1], n.incl[1], n.self[1], n.calls[0], n.incl[0],
				n.self[0], h1, h0);
		}
		out += "# edges: E caller callee calls_resim incl_resim calls_normal incl_normal\n";
		for (const auto& [k, e] : s_edges)
			out += fmt::format("E {:08X} {:08X} {} {} {} {}\n", static_cast<u32>(k >> 32), static_cast<u32>(k), e.calls[1], e.incl[1], e.calls[0], e.incl[0]);
		return out;
	}

	bool DumpFile(const std::string& path) { return FileSystem::WriteStringToFile(path.c_str(), Dump()); }
} // namespace EeCallGraph
