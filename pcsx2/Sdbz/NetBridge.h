// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <functional>
#include <string>
#include <vector>

// PovertyCaster netcode (pc_ps2bridge.dll, MinGW C++23 behind a C ABI: Sdbz/pc_ps2_bridge.h), loaded at runtime.
// GameRollback drives it once per frame boundary on the EE thread in DEFERRED dispatch: the bridge returns the
// frame's plan (optional LOAD, then ADVANCE/SAVE per re-simulated frame, then the forward frame's pair), the host
// executes it with its own page ring and answers every SAVE with a checksum.
namespace NetBridge
{
	static constexpr u32 INPUT_SIZE = 6; // pcb_ps2_input: buttons (active-high, pad bit order) + 4 stick bytes

	enum class Mode : int
	{
		SyncTest = 0, // GekkoNet stress session: 8-deep rollback every frame, checksum compare
		Local = 1,
		P2P = 2,
	};
	struct Config
	{
		Mode mode = Mode::SyncTest;
		int local_player = 0;
		std::string remote;   // "ip:port"
		u16 port = 7000;
		u8 input_delay = 0;
		std::string replay_path; // .pcrep to record (empty = none)
		std::string game_id;
	};
	struct Host
	{
		std::function<void(int player, u8* out)> poll_local_input; // INPUT_SIZE bytes
		std::function<bool()> in_game;                             // rollback phase (else lockstep)
		std::function<void(int frame, u32 local, u32 remote, int kind)> on_desync;
	};
	struct Step
	{
		s32 frame = 0;
		bool rolling_back = false;
		u8 inputs[2][INPUT_SIZE] = {};
		s32 save_index = -1; // the SAVE that follows this ADVANCE (-1 = none)
	};
	struct Plan
	{
		bool has_load = false;
		s32 load_frame = -1;
		u32 rollback_advances = 0;
		std::vector<Step> steps; // ADVANCE events in order, each with its SAVE index
	};

	bool Start(const Config& cfg, Host host, std::string* error);
	void Stop();
	bool Active();
	// EE thread: > 0 = advances in `plan`; 0 = hold (do not run a frame); < 0 = error
	int Frame(Plan* plan);
	void ResolveSave(s32 save_index, u32 checksum);
	std::string Status();
} // namespace NetBridge
