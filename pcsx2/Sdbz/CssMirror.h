// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <array>
#include <functional>
#include <string>
#include <vector>

// Async MIRROR character select for link-mode netplay (PovertyCaster lobbyCss model, like QoH99 / GD / Last Bronx):
// every PC runs its own character select; the local player drives their own side with their own pad (no latency);
// each PC broadcasts its side's state as an idempotent record over the reliable link channel; the receiving PC drives
// the remote side toward that record through the game's own input code (one-frame presses on that side's port,
// closed loop on the side's script state). Nothing but these records travels; the battle still uses the agreed
// selection written at the commit.
//
// The last lock (the one that makes both sides locked and starts the game's own "both locked" transition) is
// echo-gated so the two PCs can never disagree: a PC applies it only after the other PC has committed to its own lock,
// and a committed side cannot un-pick. Costs one round trip at the final lock only.
//
// Game knowledge comes from the manifest (link.css_mirror, see FUC notes/CSS_MIRROR_RE.md). EE thread only.
namespace CssMirror
{
	struct Config
	{
		bool enabled = false;
		u32 tick_hook = 0;          // Seq_TickThreads entry (a0 = VM): finds the character-select script object
		u32 sig_off = 0;            // script-relative offset of a signature string identifying the CSS script
		std::string sig;            //   ... and the string
		u32 scene_addr = 0, scene_value = 0;
		std::array<u32, 2> side_rec{};          // S[s]: +4 locked char (-1 = not locked), +8 colour
		std::array<u32, 2> var_col{}, var_row{}, var_chr{}, var_colour{};
		u32 var_stage = 0, var_bgm = 0, var_bgm_ok = 0, var_diarmuid = 0, var_count_a = 0, var_count_b = 0, var_stage_avail = 0;
		std::array<u32, 2> side_thread{};       // VM thread index of each side
		u32 main_thread = 1;
		std::array<u32, 2> pc_browse{}, pc_colour{}, pc_locked{}, pc_confirm{}, pc_colour_confirm{};
		u32 pc_stage = 0;
		u32 grid_table = 0, grid_table_b = 0, grid_cols = 9, grid_rows = 2, random_col = 4;
		u32 unlock_bits = 0, unlock_base = 511;
		u32 stage_entries = 10;                 // stage cursor 0..stage_entries (0 = RANDOM)
		u32 bgm_count = 29;
		u16 btn_up = 0x1000, btn_down = 0x4000, btn_left = 0x8000, btn_right = 0x2000, btn_ok = 0x20, btn_back = 0x40,
			btn_l1 = 0x4, btn_r1 = 0x8;
	};

	void Configure(const Config& cfg);
	bool Enabled();
	// session / link (re)start; local_side = this PC's player (0/1); seed = session value for owner-side RANDOM picks
	void Reset(int local_side, u32 seed);
	u32 TickHookPc();
	void OnScriptTick(u32 vm); // Seq_TickThreads hook
	// Menu frame boundary: publish our side's record (send() gets the bytes) and refresh the remote drivers.
	void Frame(u32 menu_frame, const std::function<void(const void*, u32)>& send);
	void OnMessage(const u8* data, u32 len);
	bool InCss(); // the character select is live and latched
	// Pad read in a link menu frame: `buttons` is the local player's input (PadFeed layout, active-high) for the
	// local port and ignored for the remote one. Returns the buttons the game must see on `port`.
	u16 Pad(u32 port, u16 buttons);
	std::string Status();
} // namespace CssMirror
