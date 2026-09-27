// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Sdbz/EeBlockProf.h"

#include "Host.h"
#include "R5900.h"
#include "VMManager.h"

#include "common/Console.h"
#include "common/FileSystem.h"

#include "fmt/format.h"

#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <unordered_map>

namespace EeBlockProf
{
	namespace
	{
		std::atomic<bool> s_on{false};
		std::mutex s_mtx;
		std::deque<Rec> s_store;                      // stable addresses: the JIT increments these directly
		std::unordered_map<u32, Rec*> s_recs;         // EE start pc -> counters (persists across recompiler resets)
		std::map<uptr, std::pair<u32, u32>> s_code;   // host code start -> (EE start pc, host size), current JIT only
		std::unordered_map<u32, u64> s_host[2];       // EE start pc -> host samples [normal, resim]
		u64 s_host_unknown = 0;

		void ResetRecompiler()
		{
			if (!VMManager::HasValidVM())
				return;
			Host::RunOnCPUThread([]() { Cpu->Reset(); });
		}
	} // namespace

	void Start()
	{
		{
			std::lock_guard lk(s_mtx);
			for (Rec& r : s_store)
				r.count[0] = r.count[1] = 0;
			s_host[0].clear();
			s_host[1].clear();
			s_host_unknown = 0;
		}
		s_on.store(true);
		ResetRecompiler();
		Console.WriteLn("EeBlockProf: on (recompiler flushed; every EE block counts its entries)");
	}

	void Stop()
	{
		if (!s_on.exchange(false))
			return;
		ResetRecompiler();
		Console.WriteLn("EeBlockProf: off");
	}

	bool Enabled() { return s_on.load(std::memory_order_relaxed); }

	Rec* BlockRec(u32 startpc)
	{
		std::lock_guard lk(s_mtx);
		Rec*& r = s_recs[startpc];
		if (!r)
			r = &s_store.emplace_back(Rec{{0, 0}, 0, 0});
		return r;
	}

	void NoteBlock(u32 startpc, const void* x86, u32 x86size, u32 cycles, u32 size)
	{
		std::lock_guard lk(s_mtx);
		if (Rec*& r = s_recs[startpc]; r)
		{
			r->cycles = cycles;
			r->size = size;
		}
		s_code[reinterpret_cast<uptr>(x86)] = {startpc, x86size};
	}

	void OnRecReset()
	{
		std::lock_guard lk(s_mtx);
		s_code.clear();
	}

	void HostSample(uptr rip, bool resim)
	{
		std::lock_guard lk(s_mtx);
		auto it = s_code.upper_bound(rip);
		if (it == s_code.begin())
		{
			s_host_unknown++;
			return;
		}
		--it;
		if (rip >= it->first + it->second.second)
		{
			s_host_unknown++;
			return;
		}
		s_host[resim ? 1 : 0][it->second.first]++;
	}

	std::string Dump()
	{
		std::lock_guard lk(s_mtx);
		std::map<u32, std::pair<const Rec*, u64>> rows; // sorted by pc
		for (const auto& [pc, r] : s_recs)
			rows[pc].first = r;
		for (int k = 0; k < 2; k++)
			for (const auto& [pc, n] : s_host[k])
				rows[pc];
		std::string out = fmt::format("# EeBlockProf: pc size cycles count_resim count_normal host_resim host_normal (host samples outside "
									  "known blocks: {})\n",
			s_host_unknown);
		for (const auto& [pc, v] : rows)
		{
			const Rec* r = v.first;
			const auto h1 = s_host[1].find(pc), h0 = s_host[0].find(pc);
			const u64 hr = h1 == s_host[1].end() ? 0 : h1->second, hn = h0 == s_host[0].end() ? 0 : h0->second;
			const u64 cr = r ? r->count[1] : 0, cn = r ? r->count[0] : 0;
			if (!cr && !cn && !hr && !hn)
				continue;
			out += fmt::format("{:08X} {} {} {} {} {} {}\n", pc, r ? r->size : 0, r ? r->cycles : 0, cr, cn, hr, hn);
		}
		return out;
	}

	bool DumpFile(const std::string& path) { return FileSystem::WriteStringToFile(path.c_str(), Dump()); }
} // namespace EeBlockProf
