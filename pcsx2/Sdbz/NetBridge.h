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
		Replay = 3,        // play a .pcrep (replay_path): recorded confirmed inputs + CHECK compares
		JournalReplay = 4, // play a host schedule journal (journal_path) offline: same plans, checksum compare
		Link = 5,          // a persistent peer link (bridge revision 2): async menus via messages, the rollback
		                   // session attached per battle (Attach/Detach); Frame/ResolveSave work while attached
	};
	struct Config
	{
		Mode mode = Mode::SyncTest;
		int local_player = 0;
		std::string remote;   // "ip:port"
		u16 port = 7000;
		u8 input_delay = 0;
		std::string replay_path; // .pcrep to record (P2P/SyncTest/Local) or to play (Replay)
		std::string journal_path; // host schedule journal: record (sessions) or play (JournalReplay)
		std::string game_id;
	};
	struct Host
	{
		std::function<void(int player, u8* out)> poll_local_input; // INPUT_SIZE bytes
		std::function<bool()> in_game;                             // rollback phase (else lockstep)
		std::function<void(int frame, u32 local, u32 remote, int kind)> on_desync;
		std::function<u32()> state_checksum; // the watched-state hash (replay anchor identity)
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
		std::vector<s32> pre_saves; // SAVEs before any ADVANCE (the session's frame-0 save): answer with the current state
		s32 confirmed = -1; // newest netcode frame run on real inputs from every peer (never loaded again); -1 = unknown
	};

	bool Start(const Config& cfg, Host host, std::string* error);
	void Stop();
	bool Active();
	// EE thread: > 0 = advances in `plan`; 0 = hold (do not run a frame); < 0 = error
	int Frame(Plan* plan);
	void ResolveSave(s32 save_index, u32 checksum);
	std::string Status();
	float PaceFactor(); // P2P: suggested frame-time multiplier (1.0 nominal)
	struct NetStats
	{
		s32 frame = 0, rollbacks = 0, last_rollback_frames = 0, delay = 0, stalled = 0, desync_frame = -1;
		s64 rollback_frames_total = 0;
		u32 ping_ms = 0, jitter_ms = 0, compares = 0, mismatches = 0;
		float frames_ahead = 0.0f;
		bool desynced = false;
	};
	bool GetStats(NetStats* out); // false: no session
	int InputDelay(); // configured per-battle input delay (frames)

	// ---- Link mode (Mode::Link). EE thread. ----
	struct Message
	{
		u16 type = 0;
		s32 frame = 0;
		std::vector<u8> data;
	};
	bool LinkSend(u16 type, s32 frame, const void* data, u32 len);
	// Pumps the link (resends, keepalive) and returns the next received message; false = none.
	bool LinkPoll(Message* out);
	// 0 pending (call again next vsync; do not run game frames), 1 attached (frame 0 = this frame), < 0 failed
	int Attach(u32 attach_id);
	// 0 pending (keep running Frame plans), 1 clean, 2 forced, < 0 error
	int Detach();
	bool Attached();
	bool RemoteDetachRequested();
	u32 LinkPingMs();
} // namespace NetBridge
