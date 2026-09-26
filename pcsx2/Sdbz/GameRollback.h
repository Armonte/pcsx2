// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <string>

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

	std::string Status();
	void SetFileWatch(bool on);
} // namespace GameRollback
