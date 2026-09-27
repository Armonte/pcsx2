// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <string>
#include <vector>

// EE memory-access census: while enabled, the recompiler instruments every EE load/store with native compares against
// the census ranges and records each hit as (instruction pc, read/write, rollback phase) with a count and which
// fields (offset within the range's stride, 16-byte buckets) it touched. Answers "who reads/writes this memory, and
// when" exactly (every executed access, not a sample): the reader census that proves an output is render-only before
// a resim lever skips its producer. Enabling/disabling flushes the recompiler. Slow while on; measurement only.
namespace MemCensus
{
	struct Range
	{
		u32 lo, hi;     // [lo, hi)
		u32 stride = 0; // field offset = (addr - lo) % stride (0 = addr - lo)
	};

	void Start(std::vector<Range> ranges, bool reads, bool writes); // any thread
	void Stop();
	bool Enabled();

	// recompiler / interpreter (EE thread)
	const std::vector<Range>& Ranges();
	bool WantReads();
	bool WantWrites();
	void Hit(u32 addr, u32 pc_flags); // pc | store << 31 | size_code << 28

	// "# ..." header + one line per (pc, rw, phase): pc rw phase count range lo_addr hi_addr fields(hex offsets)
	std::string Dump();
	bool DumpFile(const std::string& path);
} // namespace MemCensus
