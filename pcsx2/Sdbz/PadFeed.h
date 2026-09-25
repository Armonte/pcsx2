// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <string>
#include <vector>

// Deterministic game-side controller feed.
//
// A game hook placed right after its libpad read (e.g. FUC Pad_ReadPort's scePadRead into the raw report buffer)
// calls the rollback device with CMD_PAD_FEED; for a fed player the raw DualShock2 report is overwritten before the
// game decodes it, so edges, auto-repeat, analog and pressure are derived by the game's own code exactly as for a
// real pad. The feed is frame-indexed on the EE thread (one step per read of that player), independent of window
// focus or host input, and works in menus and in battle.
//
// Buttons use the layout games decode from the report, ((b2 << 8) | b3) ^ 0xFFFF (FUC g_Pad held):
//   L2 0x1, R2 0x2, L1 0x4, R1 0x8, TRIANGLE 0x10, CIRCLE 0x20, CROSS 0x40, SQUARE 0x80,
//   SELECT 0x100, L3 0x200, R3 0x400, START 0x800, UP 0x1000, RIGHT 0x2000, DOWN 0x4000, LEFT 0x8000.
namespace PadFeed
{
	struct Step
	{
		u16 buttons = 0;       // layout above (active-high here, written active-low into the report)
		u8 lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;
		u32 frames = 1;        // hold this step for N reads
	};

	void Off(u32 player);
	void Const(u32 player, const Step& s);
	void Sequence(u32 player, std::vector<Step> steps, bool loop);
	// Seeded random masher: every hold is a random subset of `mask` held for [min_hold, max_hold] reads.
	void Mash(u32 player, u32 seed, u16 mask, u32 min_hold, u32 max_hold);
	std::string Status();

	// EE thread (device syscall): player = port + slot, buf = raw report, ret = scePadRead's return value.
	// Returns the value the game should see from scePadRead.
	u32 OnPadRead(u32 player, u8* buf, u32 ret);
} // namespace PadFeed
