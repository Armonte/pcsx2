// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Sdbz/NetBridge.h"
#include "Sdbz/pc_ps2_bridge.h"

#include "Config.h"

#include "common/Console.h"
#include "common/DynamicLibrary.h"
#include "common/Error.h"
#include "common/Path.h"

#include "fmt/format.h"

#include <cstring>

namespace NetBridge
{
	namespace
	{
		DynamicLibrary s_lib;
		decltype(&pcb_abi_version) p_abi_version = nullptr;
		decltype(&pcb_create) p_create = nullptr;
		decltype(&pcb_destroy) p_destroy = nullptr;
		decltype(&pcb_frame) p_frame = nullptr;
		decltype(&pcb_resolve_save) p_resolve_save = nullptr;
		decltype(&pcb_get_stats) p_get_stats = nullptr;
		decltype(&pcb_last_error) p_last_error = nullptr;

		pcb_session* s_session = nullptr;
		Host s_host;
		Config s_cfg;
		std::string s_remote, s_replay, s_game; // storage the C config points into

		bool Load(std::string* error)
		{
			if (s_lib.IsOpen())
				return true;
			const std::string path = Path::Combine(EmuFolders::AppRoot, "pc_ps2bridge.dll");
			Error err;
			if (!s_lib.Open(path.c_str(), &err))
			{
				if (error)
					*error = fmt::format("cannot load {}: {}", path, err.GetDescription());
				return false;
			}
			const bool ok = s_lib.GetSymbol("pcb_abi_version", &p_abi_version) && s_lib.GetSymbol("pcb_create", &p_create) &&
							s_lib.GetSymbol("pcb_destroy", &p_destroy) && s_lib.GetSymbol("pcb_frame", &p_frame) &&
							s_lib.GetSymbol("pcb_resolve_save", &p_resolve_save) &&
							s_lib.GetSymbol("pcb_get_stats", &p_get_stats) && s_lib.GetSymbol("pcb_last_error", &p_last_error);
			if (!ok || p_abi_version() != PCB_ABI_VERSION)
			{
				if (error)
					*error = ok ? fmt::format("bridge ABI {} != {}", p_abi_version(), PCB_ABI_VERSION) : "bridge exports missing";
				s_lib.Close();
				return false;
			}
			return true;
		}

		// callbacks (EE thread, inside pcb_frame)
		void PCB_CALL CbPoll(void*, int32_t player, int32_t, uint8_t* out)
		{
			std::memset(out, 0, INPUT_SIZE);
			if (s_host.poll_local_input)
				s_host.poll_local_input(player, out);
		}
		int32_t PCB_CALL CbScene(void*)
		{
			return (!s_host.in_game || s_host.in_game()) ? PCB_SCENE_IN_GAME : PCB_SCENE_CHARA_INTRO;
		}
		void PCB_CALL CbLog(void*, int32_t level, const char* line)
		{
			if (level >= PCB_LOG_WARN)
				Console.Warning("NetBridge: %s", line);
			else
				Console.WriteLn("NetBridge: %s", line);
		}
		void PCB_CALL CbDesync(void*, const pcb_desync_info* info)
		{
			Console.Error("NetBridge: DESYNC at frame %d (local %08X remote %08X kind %d)", info->frame, info->local_checksum,
				info->remote_checksum, info->kind);
			if (s_host.on_desync)
				s_host.on_desync(info->frame, info->local_checksum, info->remote_checksum, info->kind);
		}
	} // namespace

	bool Start(const Config& cfg, Host host, std::string* error)
	{
		Stop();
		if (!Load(error))
			return false;
		s_cfg = cfg;
		s_host = std::move(host);
		s_remote = cfg.remote;
		s_replay = cfg.replay_path;
		s_game = cfg.game_id;
		pcb_config c = {};
		c.struct_size = sizeof(c);
		c.abi_version = PCB_ABI_VERSION;
		c.mode = static_cast<int32_t>(cfg.mode);
		c.dispatch = PCB_DISPATCH_DEFERRED;
		c.num_players = 2;
		c.local_player = cfg.local_player;
		c.remote_addr = s_remote.c_str();
		c.local_port = cfg.port;
		c.input_delay = cfg.input_delay;
		c.prediction_window = 8;
		c.check_distance = 8;
		c.input_size = INPUT_SIZE;
		c.state_size = 16; // token host: snapshots live in the PageSnapshotRing
		c.replay_path = s_replay.empty() ? nullptr : s_replay.c_str();
		c.game_id = s_game.c_str();
		pcb_host h = {};
		h.struct_size = sizeof(h);
		h.poll_local_input = CbPoll;
		h.scene = CbScene;
		h.log = CbLog;
		h.on_desync = CbDesync;
		s_session = p_create(&c, &h);
		if (!s_session)
		{
			if (error)
				*error = fmt::format("pcb_create: {}", p_last_error());
			return false;
		}
		Console.WriteLn("NetBridge: session started (mode %d, local player %d, delay %u)", static_cast<int>(cfg.mode),
			cfg.local_player, cfg.input_delay);
		return true;
	}

	void Stop()
	{
		if (s_session)
		{
			p_destroy(s_session);
			s_session = nullptr;
			Console.WriteLn("NetBridge: session stopped");
		}
	}

	bool Active() { return s_session != nullptr; }

	int Frame(Plan* plan)
	{
		plan->has_load = false;
		plan->load_frame = -1;
		plan->rollback_advances = 0;
		plan->steps.clear();
		if (!s_session)
			return -1;
		const pcb_plan* p = nullptr;
		const int32_t n = p_frame(s_session, &p);
		if (n <= 0 || !p)
			return n;
		for (int32_t k = 0; k < p->event_count; k++)
		{
			const pcb_event& e = p->events[k];
			if (e.type == PCB_EV_LOAD)
			{
				plan->has_load = true;
				plan->load_frame = e.frame;
			}
			else if (e.type == PCB_EV_ADVANCE)
			{
				Step s;
				s.frame = e.frame;
				s.rolling_back = e.rolling_back != 0;
				std::memcpy(s.inputs, e.inputs, sizeof(s.inputs));
				plan->steps.push_back(s);
				if (s.rolling_back)
					plan->rollback_advances++;
			}
			else if (e.type == PCB_EV_SAVE && !plan->steps.empty())
				plan->steps.back().save_index = e.save_index;
		}
		return n;
	}

	void ResolveSave(s32 save_index, u32 checksum)
	{
		if (s_session && save_index >= 0)
			p_resolve_save(s_session, save_index, pcb_finalize_checksum(checksum), 0);
	}

	std::string Status()
	{
		if (!s_session)
			return "net: off";
		pcb_stats st = {};
		st.struct_size = sizeof(st);
		p_get_stats(s_session, &st);
		return fmt::format("net: frame {} rollbacks {} (last {} deep) desync {} (frame {}) ping {} ms jitter {} ms ahead {:.2f} "
						   "delay {} compares {}/{} mismatches abandoned saves {} stalled {}",
			st.current_frame, st.rollbacks, st.last_rollback_frames, st.desynced, st.desync_frame, st.ping_ms, st.jitter_ms,
			st.frames_ahead, st.current_delay, st.health_compares, st.health_mismatches, st.deferred_abandoned,
			st.stalled_frames);
	}
} // namespace NetBridge
