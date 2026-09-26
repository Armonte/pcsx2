// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <functional>
#include <vector>

// Emulator-level EE code hooks: game instrumentation without patching game memory (no code caves, nothing in
// savestates, no per-game assembly). A hooked address always starts its own recompiled block; the recompiler emits
// the hook at the block's entry, where no guest register is cached on the host:
//   ResimGate: native x86 test of the rollback device's re-simulation flag; while set, the function returns at once
//              (pc = $ra, $v0 untouched) -- for void work that must not run in re-simulated frames (draw leaves, audio).
//   Call:      C++ handler with the guest registers in cpuRegs; it may change them and return Return to make the guest
//              function return immediately (pc = $ra, after the handler set $v0 etc.), Jump (the handler set pc) or
//              Continue. Works at any address that is not a branch delay slot (function entries, call sites, the
//              instruction after a call = post-call hook).
//   SkipCall:  at a `jal` site: skip the call (its delay slot still runs, execution continues after it) while the
//              rollback device re-simulates, or always (permanent removal of provably dead calls).
// Hooks may be added/removed at any time (hot reload): the affected blocks are dropped on the CPU thread and recompiled
// with the new hook set. The interpreter runs the same hooks.
namespace EeHooks
{
	enum class Kind : u8
	{
		None,
		ResimGate,
		Call,
		SkipCallResim,
		SkipCallAlways,
	};
	enum class Action : u8
	{
		Continue,
		Return, // pc = $ra
		Jump,   // the handler set cpuRegs.pc
	};
	using Handler = std::function<Action(u32 pc)>;

	// owner: who registered the hook; Clear(owner) drops only that owner's hooks (a Lua reload must not drop the
	// manifest-driven rollback's hooks and vice versa).
	enum Owner : u8
	{
		OWNER_SCRIPT = 0,
		OWNER_GAME = 1,
	};
	void AddResimGate(u32 pc, Owner owner = OWNER_SCRIPT);
	void AddSkipCall(u32 site, bool always, Owner owner = OWNER_SCRIPT);
	void AddCall(u32 pc, Handler handler, Owner owner = OWNER_SCRIPT);
	// Same, but the handler only runs when $ra is one of ra_filter (callers' return addresses): the comparisons are
	// emitted natively, so other callers of a hot function pay a few compares, not a C++ call.
	void AddCallFiltered(u32 pc, Handler handler, std::vector<u32> ra_filter, Owner owner = OWNER_SCRIPT);
	void Remove(u32 pc);
	void Clear(Owner owner = OWNER_SCRIPT);

	// recompiler / interpreter side (EE thread)
	Kind Lookup(u32 pc);          // cheap: page bitmap first
	Action RunCall(u32 pc);       // Kind::Call (applies the $ra filter)
	std::vector<u32> CallFilter(u32 pc); // empty = no filter
	const u8* ResimFlag();        // byte the ResimGate tests (nonzero while re-simulating)
	u64* GateReturnCounter();     // incremented natively each time a ResimGate returns early
	u64 GateReturns();
} // namespace EeHooks
