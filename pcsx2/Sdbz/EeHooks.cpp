// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Sdbz/EeHooks.h"
#include "Sdbz/RollbackDevice.h"

#include "Host.h"
#include "R5900.h"
#include "VMManager.h"

#include <array>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace EeHooks
{
	namespace
	{
		struct Hook
		{
			Kind kind = Kind::None;
			Handler handler;
			Owner owner = OWNER_SCRIPT;
			std::vector<u32> ra_filter;
			u32 v0 = 0; // ResimGateRet
		};
		std::mutex s_mtx;
		std::unordered_map<u32, Hook> s_hooks;
		std::array<std::atomic<u16>, (0x02000000u >> 12)> s_page_count{}; // hooks per 4 KiB page (fast negative lookup)

		u32 Key(u32 pc) { return pc & 0x01FFFFFCu; }

		// Drop the recompiled code containing pc so the next execution recompiles with the current hook set.
		void Invalidate(u32 pc)
		{
			if (VMManager::HasValidVM())
				Host::RunOnCPUThread([pc]() { Cpu->Clear(pc & ~3u, 1); });
		}

		void Set(u32 pc, Hook h)
		{
			const u32 k = Key(pc);
			{
				std::lock_guard lk(s_mtx);
				const bool had = s_hooks.count(k) != 0;
				s_hooks[k] = std::move(h);
				if (!had)
					s_page_count[k >> 12].fetch_add(1, std::memory_order_relaxed);
			}
			Invalidate(pc);
		}
	} // namespace

	void AddResimGate(u32 pc, Owner owner) { Set(pc, {Kind::ResimGate, {}, owner}); }
	void AddResimGateRet(u32 pc, u32 v0, Owner owner) { Set(pc, {Kind::ResimGateRet, {}, owner, {}, v0}); }
	u32 GateReturnValue(u32 pc)
	{
		std::lock_guard lk(s_mtx);
		const auto it = s_hooks.find(Key(pc));
		return it == s_hooks.end() ? 0 : it->second.v0;
	}
	void AddCall(u32 pc, Handler handler, Owner owner) { Set(pc, {Kind::Call, std::move(handler), owner}); }
	void AddCallFiltered(u32 pc, Handler handler, std::vector<u32> ra_filter, Owner owner)
	{
		Set(pc, {Kind::Call, std::move(handler), owner, std::move(ra_filter)});
	}
	void AddSkipCall(u32 site, bool always, Owner owner)
	{
		Set(site, {always ? Kind::SkipCallAlways : Kind::SkipCallResim, {}, owner});
	}

	void Remove(u32 pc)
	{
		const u32 k = Key(pc);
		{
			std::lock_guard lk(s_mtx);
			if (s_hooks.erase(k) == 0)
				return;
			s_page_count[k >> 12].fetch_sub(1, std::memory_order_relaxed);
		}
		Invalidate(pc);
	}

	void Clear(Owner owner)
	{
		std::vector<u32> pcs;
		{
			std::lock_guard lk(s_mtx);
			for (auto it = s_hooks.begin(); it != s_hooks.end();)
			{
				if (it->second.owner != owner)
				{
					++it;
					continue;
				}
				pcs.push_back(it->first);
				s_page_count[it->first >> 12].fetch_sub(1, std::memory_order_relaxed);
				it = s_hooks.erase(it);
			}
		}
		for (const u32 pc : pcs)
			Invalidate(pc);
	}

	Kind Lookup(u32 pc)
	{
		const u32 k = Key(pc);
		if (s_page_count[k >> 12].load(std::memory_order_relaxed) == 0)
			return Kind::None;
		std::lock_guard lk(s_mtx);
		const auto it = s_hooks.find(k);
		return it == s_hooks.end() ? Kind::None : it->second.kind;
	}

	Action RunCall(u32 pc)
	{
		Handler h;
		{
			std::lock_guard lk(s_mtx);
			const auto it = s_hooks.find(Key(pc));
			if (it == s_hooks.end() || it->second.kind != Kind::Call)
				return Action::Continue;
			if (!it->second.ra_filter.empty())
			{
				const u32 ra = cpuRegs.GPR.n.ra.UL[0];
				bool hit = false;
				for (const u32 r : it->second.ra_filter)
					hit |= (r == ra);
				if (!hit)
					return Action::Continue;
			}
			h = it->second.handler;
		}
		return h ? h(pc) : Action::Continue;
	}

	std::vector<u32> CallFilter(u32 pc)
	{
		std::lock_guard lk(s_mtx);
		const auto it = s_hooks.find(Key(pc));
		return it == s_hooks.end() ? std::vector<u32>{} : it->second.ra_filter;
	}

	const u8* ResimFlag() { return RollbackDevice::ResimulatingFlag(); }

	namespace
	{
		u64 s_gate_returns = 0;
	}
	u64* GateReturnCounter() { return &s_gate_returns; }
	u64 GateReturns() { return s_gate_returns; }
} // namespace EeHooks
