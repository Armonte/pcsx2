// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <string>

// EE call-graph profiler. While enabled, the recompiler emits a shadow-call-stack push at every jal/jalr (callee,
// return address) and a pop at every `jr $ra` (matched by return address, so tail calls / unwinds stay consistent).
// Per EE function: exact call counts, inclusive and self EE cycles (normal / re-simulated). The RbProfiler sampler
// reads the shadow stack of the suspended EE thread, so every EE function also gets inclusive and self HOST time.
// Enabling/disabling flushes the recompiler. Measurement only (a C++ call per EE call/return while on).
namespace EeCallGraph
{
	void Start(); // any thread
	void Stop();
	bool Enabled();

	// recompiler (EE thread)
	void Push(u32 target, u32 ret);
	void Pop(u32 target);

	// sampler thread (EE thread suspended)
	void HostSample(bool resim);

	// "# ..." header + one line per function: fn calls_resim calls_normal incl_resim self_resim incl_normal self_normal
	//   host_incl_resim host_self_resim host_incl_normal host_self_normal
	std::string Dump();
	bool DumpFile(const std::string& path);
} // namespace EeCallGraph
