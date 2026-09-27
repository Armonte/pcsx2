// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <string>
#include <vector>

// Manifest-driven in-engine rollback for a PS2 game (pcsx2/Sdbz/ARCHITECTURE.md).
//
// A per-game YAML manifest declares the game state (regions, excludes, compare-ignores, audio ranges restored around
// re-simulation, dynamic ranges found by walking tables / lists / pointer chains), the frame (the sim-tick call site
// and the list of game functions one re-simulated frame runs), and the hooks (resim gates, call-site skips, pad
// feed, sound-only RNG draws, render-pass markers, entry actions). Everything is installed as emulator-level EE hooks
// (Sdbz/EeHooks): no game memory is patched and no EE code is written.
//
// Re-simulation is driven from the host: the hook on the sim-tick call site asks the RollbackDevice how many frames
// to re-simulate, then runs each step by setting pc to the game function and $ra to a return address (dead code,
// hooked); the hook there moves to the next step. When the list is done, the original call runs.
//
// Hot reload: the manifest file is watched; a change is applied at the next frame boundary (hooks re-registered,
// the device restarted in the same mode).
namespace GameRollback
{
	// Load the manifest and install the always-on hooks (pad feed). Any thread.
	bool Attach(const std::string& manifest_path, std::string* error = nullptr);
	void Detach();
	bool IsAttached();

	// mode: RollbackDevice::Mode (1 capture, 2 sync test, 3 bench); frames: rollback depth.
	bool Start(int mode, u32 frames, std::string* error = nullptr);
	void Stop();
	int RunningMode();
	void OnStateLoaded(); // CPU thread, after EE memory was replaced by a savestate

	// Netplay through PovertyCaster (Sdbz/NetBridge): mode 0 sync test (GekkoNet stress), 1 local, 2 p2p.
	bool NetStart(int mode, int local_player, const std::string& remote, u16 port, u8 delay, const std::string& replay,
		const std::string& journal,
		std::string* error = nullptr);
	void NetStop();
	void SessionLocks(bool on);
	void LinkDumpAtAttach(const std::string& prefix); // link mode: EE RAM dump at every battle attach (prefix.genN) // manifest session lock-down outside netplay (harness tests)
	std::string NetStatus();
	void PollAutoStart(); // CPU thread, each presented frame: launcher environment (PS2RB_*), once

	std::string Status();
	// netplay session badge for the on-screen overlay ("P1 | battle 3 | ping 105 ms"), empty outside a session. Any thread.
	std::string LinkBadge();
	// Game task profiler: inclusive EE cycles + host time per task function, split normal / re-simulated frames.
	// call_pc = the dispatcher's jalr (task fn in fn_reg), ret_pc = the instruction after it (FUC 0x211230/0x211238, $v1).
	// vm_fns: task functions whose data+8 is a Seq VM: their cost is split per script (keyed by the script base).
	bool TaskProfStart(u32 call_pc, u32 ret_pc, u32 fn_reg, std::vector<u32> vm_fns = {});
	void TaskProfStop();
	// Script-op profiler: inclusive cost per (script, opcode, sub-op) at the VM's op dispatch jalr
	// (FUC Seq_TickThreads 0x222F74 / return 0x222F7C, VM in vm_reg, instruction pointer in ip_reg).
	bool OpProfStart(u32 call_pc, u32 ret_pc, u32 vm_reg, u32 ip_reg);
	void OpProfStop();
	std::string OpProfReport(u32 top_n);
	// Call-site profiler: inclusive cost of each listed jal/jalr site (hooked at the site and at site+8).
	bool CallProfStart(const std::vector<u32>& sites, const std::vector<u32>& vt_sites = {}, const std::vector<u32>& obj_sites = {});
	void CallProfStop();
	std::string CallProfReport();
	std::string TaskProfReport(u32 top_n);
	void SetFileWatch(bool on);
} // namespace GameRollback
