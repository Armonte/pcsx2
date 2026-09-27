// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <string>

// EE basic-block profiler (mid-function granularity). While enabled, the EE recompiler emits a counter increment at
// the entry of every block it compiles (normal / re-simulated split on the rollback resim flag), and records each
// block's host code range so the RbProfiler sampler can attribute host time to the exact EE block it lands in.
// Enabling/disabling flushes the recompiler so every block is rebuilt with/without the counter.
namespace EeBlockProf
{
	struct Rec
	{
		u64 count[2]; // [0] normal, [1] re-simulated (index = resim flag byte)
		u32 cycles;   // EE cycles the recompiler charges for one pass of the block
		u32 size;     // instructions
	};

	void Start(); // any thread
	void Stop();
	bool Enabled(); // recompiler: emit counters?

	// recompiler hooks (EE thread)
	Rec* BlockRec(u32 startpc);
	void NoteBlock(u32 startpc, const void* x86, u32 x86size, u32 cycles, u32 size);
	void OnRecReset();

	// sampler thread: a sample whose host RIP lies in EE recompiled code
	void HostSample(uptr rip, bool resim);

	// "# ..." header + one line per block: pc size cycles count_resim count_normal host_resim host_normal
	std::string Dump();
	bool DumpFile(const std::string& path);
} // namespace EeBlockProf
