// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <string>

// In-process sampling profiler for the rollback path (no admin rights / ETW needed).
//
// A sampler thread suspends the EE (CPU) thread at a fixed rate, reads its instruction pointer and resumes it. Each
// sample is tagged with the rollback phase the EE thread is in (set by RollbackDevice) and bucketed by code region:
// recompiled EE / IOP / microVU / VIF-unpack / software-renderer code, or a PCSX2 function resolved through the PDB.
// The report gives, per phase, where the host time goes (normal sim, re-simulation, snapshot capture/load, render...).
namespace RbProfiler
{
	enum Phase : u8
	{
		PH_OTHER = 0,       // outside the rollback command points (present, vsync wait, frame pacing, menus)
		PH_FRAME_BEGIN,     // FRAME_BEGIN bookkeeping (gate hash, input record)
		PH_CAPTURE,         // snapshot capture of the current frame
		PH_LOAD,            // snapshot load (rollback)
		PH_SYNCTEST,        // sync-test reference copy + compare (test only)
		PH_RESIM,           // re-simulated frame (game code)
		PH_RESIM_CAPTURE,   // snapshot capture after a re-simulated frame
		PH_SIM,             // normal frame: sim tick + event pass
		PH_RENDER,          // normal frame: render passes
		PH_COUNT
	};

	// EE thread only: registers the calling thread as the sampled one (first call) and sets the current phase.
	void SetPhase(Phase p);

	void Start(u32 hz); // any thread
	void Stop();
	bool IsRunning();
	std::string Report(u32 top_n); // any thread; resolves symbols
} // namespace RbProfiler
