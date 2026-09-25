// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <string>

// In-engine rollback device -- the PCSX2 counterpart of Slippi's EXI device (EXI_DeviceSlippi).
//
// The game drives rollback itself (Slippi-style): a small MIPS routine hooked in place of the game's per-frame
// sim-tick call talks to this device through `syscall` with $v1 = MAGIC (a0 = command, a1 = argument, result in
// $v0), handled synchronously on the EE thread. At the command points the EE sits at the same place in the main
// loop every frame, so restoring game memory (PageSnapshotRing) is enough -- no CPU/emulator state is saved.
//
// Per frame the game routine does:
//   R = CMD_FRAME_BEGIN              device records this frame's input block, captures snapshot F; returns the
//                                    number of frames R to re-simulate (having already restored snapshot F-R)
//   repeat i = 0..R-1:
//     CMD_RESIM_PRE(i)               device writes the input block recorded for frame F-R+i
//     <sim tick> <event pass>        the game's own simulation, no rendering
//     CMD_RESIM_POST(i)              device captures snapshot F-R+i+1
//   CMD_CUR_PRE                      device restores this frame's input block (and in sync-test mode compares the
//                                    re-simulated state with the state captured before the rollback)
//   <sim tick>                       normal frame continues (event pass, rendering, present)
//
// Modes: Off (every command is a no-op returning 0), Capture (snapshots only), SyncTest (GGPO-style: every frame,
// roll back N frames and re-simulate with the recorded inputs, then byte-compare against the pre-rollback state and
// report every differing range -- this is how render->sim couplings and missing excludes are found).
namespace RollbackDevice
{
	static constexpr u32 MAGIC = 0x5DB2F00Du; // low byte must not be 0x64/0x68 (recompiler FlushCache shortcut)

	enum Command : u32
	{
		CMD_FRAME_BEGIN = 1,
		CMD_RESIM_PRE = 2,
		CMD_RESIM_POST = 3,
		CMD_CUR_PRE = 4,
		CMD_RENDER_BEGIN = 5, // game is about to run its render passes: switch g_RandSeed to the render stream
		CMD_RENDER_END = 6,   // render passes done: save the render stream, restore the simulation stream
		CMD_RNG_TRACE = 16,   // 16 + rng function id, $a1 = caller return address (desync attribution)
	};

	enum class Mode : int
	{
		Off = 0,
		Capture = 1,
		SyncTest = 2,
	};

	// EE thread, from the SYSCALL interpreter handler. Returns the value for $v0.
	u64 HandleSyscall(u32 cmd, u32 arg);

	void OnVMShutdown();

	// Configuration (any thread; applied by Start()).
	void ClearConfig();
	void AddRegion(u32 addr, u32 len);        // memory that makes up the game state
	void AddExclude(u32 addr, u32 len);       // never rolled back (library/driver/audio state, counters)
	void SetInputBlock(u32 addr, u32 len);    // per-frame input state recorded/injected by the device
	void AddCompareIgnore(u32 addr, u32 len); // sync test: state known not to matter (render-only), not reported
	void AddWatch(u32 addr, u32 len, const std::string& name); // sync test: named sim state, reported by name
	// Rollback gate (Slippi only rolls back during active gameplay): a u32 counter that advances only while the game
	// simulation is live (e.g. the battle frame counter). A rollback happens only if it advanced on every frame of the
	// window; menus, pauses, round transitions and loads (async IO that must not be rewound) are never rolled back.
	void SetGate(u32 counter_addr);
	// Rollback gate, I/O side: ranges that change when the game submits asynchronous I/O the simulation later waits on
	// (file loads, voice/BGM streams). A rollback window in which any of them changed is not rolled back: re-simulating
	// the submitting frame would issue the request a second time against un-rewound I/O state.
	void AddGateStable(u32 addr, u32 len);
	// RNG call tracing (game-side trampolines on the RNG functions report each call): the device records the ordered
	// (function, caller) list of every frame's normal simulation step, compares each re-simulation of that frame against
	// it, and reports the first differing call. Calls made outside both the simulation step and the render section are
	// counted separately (they advance the simulation stream behind re-simulation's back).
	void SetRngTrace(bool on);
	// Split RNG: the game's single RNG state is also consumed by rendering (HUD flicker, particle jitter, script event
	// handlers on draw passes). Re-simulation doesn't render, so the simulation stream would drift. With this set, the
	// device swaps in a separate render stream between CMD_RENDER_BEGIN/END; the simulation stream then advances only
	// in the simulation, identically in normal frames and in re-simulation. Active whenever the device is started
	// (Capture or SyncTest), so every netplay peer runs the same split.
	void SetRngSplit(u32 seed_addr, u32 size);

	void Start(Mode mode, u32 rollback_frames, bool write_protect);
	void Stop();
	std::string Status();
	std::string ReportText(); // per-page diff histogram + last differing runs (sync test)
} // namespace RollbackDevice
