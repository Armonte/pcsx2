// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <string>

// Phase-0 rollback determinism harness (sdbz/SDBZ_ROLLBACK_PLAN.md).
//
// Flow: rollback.baseline() snapshots the game-memory regions (SdbzSavestate) and starts
// recording per-frame pad blocks + chunked state checksums. rollback.replay() restores the
// snapshot, patches the game's pad fetch off (jal Pad_FetchRawState -> li $v0,1) so the
// recorded raw inputs stay authoritative, replays the recorded pads, and compares each frame's
// chunk checksums against the recording. Any divergence = memory that must join the savestate
// region list or its exclude list -- reported at 64KB-chunk granularity (Slippi hunted their
// exclude list exactly this way, minus the tooling).
//
// All controls are queued and executed on the EE/CPU thread at VSyncStart (frame settled).
namespace SdbzDeterminism
{
	// EE/CPU thread, called from Counters VSyncStart right after ScriptOverlay::CaptureOnEEThread().
	void OnVSyncStart();

	// Controls (safe from any thread; take effect on the next VSyncStart)
	void CaptureBaseline();
	void StartReplay();
	void Stop();
	std::string Status();

	// Config (script-side game knowledge; defaults = SLUS-21442). Applied at the next baseline().
	void SetRegion(u32 dataStart, u32 heapEnd); // heapEnd==0 -> read g_HeapHighWaterEnd live
	void AddExclude(u32 addr, u32 len);
	void ClearExcludes();   // resets to the built-in defaults
	void SetChunkKB(u32 kb); // checksum granularity (default 64)
	void SetMaxFrames(u32 frames); // recording cap (default 3600 = 60s)
} // namespace SdbzDeterminism
