// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Sdbz/RbProfiler.h"

#include "Memory.h"
#include "R5900.h"

#include "common/Console.h"

#include "fmt/format.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#include <dbghelp.h>
#include <timeapi.h>
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "winmm.lib")
#endif

namespace RbProfiler
{
	namespace
	{
		constexpr const char* PHASE_NAMES[PH_COUNT] = {"other (present/vsync/pacing)", "frame_begin", "capture", "load",
			"synctest (test-only)", "resim (game)", "resim capture", "sim (normal frame)", "render"};

		std::atomic<u8> s_phase{PH_OTHER};
		std::atomic<u32> s_ee_tid{0};
		std::atomic<bool> s_running{false};
		std::thread s_thread;
		std::mutex s_mtx; // guards s_hits / s_phase_samples
		std::unordered_map<u64, u64> s_hits; // (phase << 56) | rip -> samples
		std::unordered_map<u64, u64> s_ee_pc; // (phase << 32) | EE pc -> samples taken in EE recompiled code
		uptr s_ee_rec_a = 0, s_ee_rec_b = 0;
		u64 s_phase_samples[PH_COUNT] = {};
		u64 s_total = 0;

		struct Region
		{
			const char* name;
			uptr a, b;
		};

		std::vector<Region> JitRegions()
		{
			return {
				{"[JIT] EE recompiled code", reinterpret_cast<uptr>(SysMemory::GetEERec()), reinterpret_cast<uptr>(SysMemory::GetEERecEnd())},
				{"[JIT] IOP recompiled code", reinterpret_cast<uptr>(SysMemory::GetIOPRec()), reinterpret_cast<uptr>(SysMemory::GetIOPRecEnd())},
				{"[JIT] microVU0", reinterpret_cast<uptr>(SysMemory::GetVU0Rec()), reinterpret_cast<uptr>(SysMemory::GetVU0RecEnd())},
				{"[JIT] microVU1", reinterpret_cast<uptr>(SysMemory::GetVU1Rec()), reinterpret_cast<uptr>(SysMemory::GetVU1RecEnd())},
				{"[JIT] VIF unpack", reinterpret_cast<uptr>(SysMemory::GetVIFUnpackRec()), reinterpret_cast<uptr>(SysMemory::GetVIFUnpackRecEnd())},
				{"[JIT] GS software renderer", reinterpret_cast<uptr>(SysMemory::GetSWRec()), reinterpret_cast<uptr>(SysMemory::GetSWRecEnd())},
			};
		}

#ifdef _WIN32
		void SamplerLoop(u32 hz)
		{
			const DWORD tid = s_ee_tid.load();
			HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
			if (!th)
			{
				Console.Error("RbProfiler: OpenThread(%u) failed", tid);
				s_running = false;
				return;
			}
			timeBeginPeriod(1);
			const u32 period_us = std::max<u32>(1000000u / std::max<u32>(hz, 1u), 100u);
			LARGE_INTEGER freq, next, now;
			QueryPerformanceFrequency(&freq);
			QueryPerformanceCounter(&next);
			while (s_running.load(std::memory_order_relaxed))
			{
				next.QuadPart += freq.QuadPart * period_us / 1000000;
				for (;;)
				{
					QueryPerformanceCounter(&now);
					const s64 left_us = (next.QuadPart - now.QuadPart) * 1000000 / freq.QuadPart;
					if (left_us <= 0)
						break;
					if (left_us > 1500)
						Sleep(1);
					else
						YieldProcessor();
				}
				if (SuspendThread(th) == static_cast<DWORD>(-1))
					continue;
				CONTEXT ctx = {};
				ctx.ContextFlags = CONTEXT_CONTROL;
				const bool ok = GetThreadContext(th, &ctx) != 0;
				const u8 ph = s_phase.load(std::memory_order_relaxed);
				const u32 ee_pc = cpuRegs.pc; // last block entry the recompiler stored (EE thread is suspended)
				ResumeThread(th);
				if (!ok)
					continue;
				std::lock_guard lk(s_mtx);
				s_hits[(static_cast<u64>(ph) << 56) | (ctx.Rip & 0x00FFFFFFFFFFFFFFull)]++;
				s_phase_samples[ph]++;
				s_total++;
				if (ctx.Rip >= s_ee_rec_a && ctx.Rip < s_ee_rec_b)
					s_ee_pc[(static_cast<u64>(ph) << 32) | ee_pc]++;
			}
			timeEndPeriod(1);
			CloseHandle(th);
		}

		std::string Symbolize(uptr rip)
		{
			static bool init = false;
			if (!init)
			{
				SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
				SymInitialize(GetCurrentProcess(), nullptr, TRUE);
				init = true;
			}
			alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 512];
			SYMBOL_INFO* si = reinterpret_cast<SYMBOL_INFO*>(buf);
			si->SizeOfStruct = sizeof(SYMBOL_INFO);
			si->MaxNameLen = 511;
			DWORD64 disp = 0;
			if (SymFromAddr(GetCurrentProcess(), rip, &disp, si))
				return si->Name;
			HMODULE mod = nullptr;
			if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCSTR>(rip), &mod) && mod)
			{
				char name[MAX_PATH];
				if (GetModuleFileNameA(mod, name, MAX_PATH))
				{
					const char* base = std::max(std::strrchr(name, '\\'), std::strrchr(name, '/'));
					return fmt::format("[{}]", base ? base + 1 : name);
				}
			}
			return "[unknown]";
		}
#endif
	} // namespace

	void SetPhase(Phase p)
	{
#ifdef _WIN32
		if (!s_ee_tid.load(std::memory_order_relaxed))
			s_ee_tid = GetCurrentThreadId();
#endif
		s_phase.store(p, std::memory_order_relaxed);
	}

	void Start(u32 hz)
	{
#ifdef _WIN32
		Stop();
		if (!s_ee_tid.load())
		{
			Console.Error("RbProfiler: EE thread unknown (no rollback command seen yet)");
			return;
		}
		{
			std::lock_guard lk(s_mtx);
			s_hits.clear();
			s_ee_pc.clear();
			s_ee_rec_a = reinterpret_cast<uptr>(SysMemory::GetEERec());
			s_ee_rec_b = reinterpret_cast<uptr>(SysMemory::GetEERecEnd());
			std::fill(std::begin(s_phase_samples), std::end(s_phase_samples), 0);
			s_total = 0;
		}
		s_running = true;
		s_thread = std::thread(SamplerLoop, hz);
		Console.WriteLn("RbProfiler: sampling EE thread %u at %u Hz", s_ee_tid.load(), hz);
#else
		(void)hz;
		Console.Error("RbProfiler: only implemented on Windows");
#endif
	}

	void Stop()
	{
		s_running = false;
		if (s_thread.joinable())
			s_thread.join();
	}

	bool IsRunning() { return s_running.load(); }

	std::string Report(u32 top_n)
	{
#ifdef _WIN32
		std::unordered_map<u64, u64> hits, ee_pc;
		u64 phase_samples[PH_COUNT];
		u64 total;
		{
			std::lock_guard lk(s_mtx);
			hits = s_hits;
			ee_pc = s_ee_pc;
			std::copy(std::begin(s_phase_samples), std::end(s_phase_samples), phase_samples);
			total = s_total;
		}
		const std::vector<Region> jit = JitRegions();
		// per phase: bucket name -> samples (JIT regions by region, native code by symbol)
		std::map<std::string, u64> by_phase[PH_COUNT];
		std::unordered_map<uptr, std::string> symcache;
		for (const auto& [key, n] : hits)
		{
			const u8 ph = static_cast<u8>(key >> 56);
			const uptr rip = static_cast<uptr>(key & 0x00FFFFFFFFFFFFFFull);
			std::string name;
			for (const Region& r : jit)
			{
				if (rip >= r.a && rip < r.b)
				{
					name = r.name;
					break;
				}
			}
			if (name.empty())
			{
				auto it = symcache.find(rip);
				if (it == symcache.end())
					it = symcache.emplace(rip, Symbolize(rip)).first;
				name = it->second;
			}
			if (ph < PH_COUNT)
				by_phase[ph][name] += n;
		}
		std::string out = fmt::format("# RbProfiler: {} samples of the EE thread\n\n## phases (share of EE-thread time)\n", total);
		for (u32 p = 0; p < PH_COUNT; p++)
			out += fmt::format("{:6.2f}%  {:8}  {}\n", total ? 100.0 * phase_samples[p] / total : 0.0, phase_samples[p], PHASE_NAMES[p]);
		for (u32 p = 0; p < PH_COUNT; p++)
		{
			if (!phase_samples[p])
				continue;
			std::vector<std::pair<u64, std::string>> v;
			for (const auto& [name, n] : by_phase[p])
				v.push_back({n, name});
			std::sort(v.rbegin(), v.rend());
			out += fmt::format("\n## {} ({} samples)\n", PHASE_NAMES[p], phase_samples[p]);
			for (u32 i = 0; i < v.size() && i < top_n; i++)
				out += fmt::format("{:6.2f}%  {:8}  {}\n", 100.0 * v[i].first / phase_samples[p], v[i].first, v[i].second);
		}
		// EE code hot spots (cpuRegs.pc = block entry; symbolize offline against the game's IDB)
		for (u32 p = 0; p < PH_COUNT; p++)
		{
			std::vector<std::pair<u64, u32>> v;
			u64 n_ee = 0;
			for (const auto& [key, n] : ee_pc)
			{
				if ((key >> 32) == p)
				{
					v.push_back({n, static_cast<u32>(key)});
					n_ee += n;
				}
			}
			if (v.empty())
				continue;
			std::sort(v.rbegin(), v.rend());
			out += fmt::format("\n## EE blocks in {} ({} samples in EE JIT; pc = recompiled block entry)\n", PHASE_NAMES[p], n_ee);
			for (u32 i = 0; i < v.size() && i < top_n * 2; i++)
				out += fmt::format("{:6.2f}%  {:8}  ee_pc {:08X}\n", 100.0 * v[i].first / n_ee, v[i].first, v[i].second);
		}
		return out;
#else
		(void)top_n;
		return "RbProfiler: only implemented on Windows\n";
#endif
	}
} // namespace RbProfiler
