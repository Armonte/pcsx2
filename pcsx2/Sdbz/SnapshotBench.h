// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <string>

// Lua-driven driver for PageSnapshotRing (Lua table `snap`): configure regions per game, capture
// every frame at the EE frame boundary, roll back N frames, and optionally verify every capture /
// load byte-for-byte against RAM (catches a dirty page the tracker missed). Controls are queued
// and run on the EE/CPU thread at VSyncStart, like SdbzDeterminism.
namespace SnapshotBench
{
	void OnVSyncStart(); // EE/CPU thread, Counters VSyncStart
	void OnVMShutdown();

	// Region/exclude setup (applied by Start()).
	void ClearRegions();
	void AddRegion(u32 addr, u32 len);
	void AddExclude(u32 addr, u32 len);

	void Start(u32 capacity, bool write_protect); // builds the ring, begins per-frame capture
	void Stop();
	void Rollback(u32 frames_back);  // restore the snapshot taken frames_back captures ago
	void SetVerify(bool enabled);
	std::string Status();
} // namespace SnapshotBench
