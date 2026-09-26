// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"
#include "common/Timer.h"

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#endif

// Thread CPU-time stopwatch (Windows: QueryThreadCycleTime). Unlike wall time it doesn't count time the thread was
// preempted, so rollback/capture costs stay comparable when the host is busy with other work. Reported in
// microseconds via a one-time cycles-per-microsecond calibration. Other platforms fall back to wall time.
class CpuTimer
{
public:
	CpuTimer() { Reset(); }

	void Reset() { m_start = Now(); }

	double GetTimeMicroseconds() const
	{
#ifdef _WIN32
		return static_cast<double>(Now() - m_start) / CyclesPerMicrosecond();
#else
		return static_cast<double>(Now() - m_start) / 1000.0;
#endif
	}

	double GetTimeNanoseconds() const { return GetTimeMicroseconds() * 1000.0; }

private:
	static u64 Now()
	{
#ifdef _WIN32
		ULONG64 c = 0;
		QueryThreadCycleTime(GetCurrentThread(), &c);
		return c;
#else
		return static_cast<u64>(Common::Timer::GetCurrentValue());
#endif
	}

#ifdef _WIN32
	static double CyclesPerMicrosecond()
	{
		static const double cpu = []() {
			// calibrate the cycle counter against wall time on this (busy-spinning) thread
			Common::Timer t;
			ULONG64 c0 = 0, c1 = 0;
			QueryThreadCycleTime(GetCurrentThread(), &c0);
			while (t.GetTimeNanoseconds() < 20000000.0) {}
			QueryThreadCycleTime(GetCurrentThread(), &c1);
			return static_cast<double>(c1 - c0) / (t.GetTimeNanoseconds() / 1000.0);
		}();
		return cpu;
	}
#endif

	u64 m_start;
};
