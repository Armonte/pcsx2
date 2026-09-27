// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <string>
#include <vector>

// Native (C++) implementations of hot EE leaf functions, installed as EeHooks::Native at the function entry. Each is a
// straight transliteration of the game's MIPS code using the recompiler's exact per-op semantics (EE FPU: PS2 add/sub
// guard-bit emulation + clamps; VU0 macro: separate mul/add + operand clamps; MXCSR is the EE's, inherited from the JIT)
// and leaves memory AND the registers the EE code writes (GPR/FPR/ACC/VF) exactly as the EE code would, so the choice
// native vs EE is invisible to the game. The synctest proves exactness when a native runs only on (A/B) re-simulated
// frames: any bit difference is a SIM DESYNC. A native declines (EE code runs) when an operand is outside main RAM,
// VU0 is busy, or the clamp configuration is not the one it implements.
namespace EeNative
{
	enum Mode : u8
	{
		MODE_ALWAYS = 0,   // every frame
		MODE_RESIM = 1,    // re-simulated frames only
		MODE_RESIM_AB = 2, // A/B "on" re-simulated frames only (exactness + speed measurement)
	};
	// name -> implementation (e.g. "vu0_mat44_mul"); false if unknown
	bool Install(u32 pc, const std::string& impl, Mode mode, std::string* error);
	void RemoveAll();
	std::vector<std::string> Implementations();
	std::string Stats(); // calls taken / declined per installed native
} // namespace EeNative
