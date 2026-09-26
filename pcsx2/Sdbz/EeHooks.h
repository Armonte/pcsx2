// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <functional>

// Emulator-level EE code hooks: game instrumentation without patching game memory (no code caves, nothing in
// savestates, no per-game assembly). A hooked address always starts its own recompiled block; the recompiler emits
// the hook at the block's entry, where no guest register is cached on the host:
//   ResimGate: native x86 test of the rollback device's re-simulation flag; while set, the function returns at once
//              (pc = $ra, $v0 untouched) -- for void work that must not run in re-simulated frames (draw leaves, audio).
//   Call:      C++ handler with the guest registers in cpuRegs; it may change them and return Return to make the guest
//              function return immediately (pc = $ra, after the handler set $v0 etc.) or Continue.
// Hooks may be added/removed at any time (hot reload): the affected blocks are dropped on the CPU thread and recompiled
// with the new hook set. The interpreter runs the same hooks.
namespace EeHooks
{
	enum class Kind : u8
	{
		None,
		ResimGate,
		Call,
	};
	enum class Action : u8
	{
		Continue,
		Return,
	};
	using Handler = std::function<Action(u32 pc)>;

	void AddResimGate(u32 pc);
	void AddCall(u32 pc, Handler handler);
	void Remove(u32 pc);
	void Clear();

	// recompiler / interpreter side (EE thread)
	Kind Lookup(u32 pc);          // cheap: page bitmap first
	Action RunCall(u32 pc);       // Kind::Call
	const u8* ResimFlag();        // byte the ResimGate tests (nonzero while re-simulating)
} // namespace EeHooks
