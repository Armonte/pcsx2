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

#include <cstdio>
#include <cstring>
#include <map>

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
		decltype(&pcb_replay_open) p_replay_open = nullptr;
		decltype(&pcb_replay_close) p_replay_close = nullptr;
		decltype(&pcb_replay_frame) p_replay_frame = nullptr;
		decltype(&pcb_replay_resolve_save) p_replay_resolve_save = nullptr;
		decltype(&pcb_replay_get_stats) p_replay_get_stats = nullptr;
		pcb_replay* s_replay_h = nullptr;

		// Host schedule journal: one text record per planned frame and per answered save. A session writes it; the
		// JournalReplay source plays the same plans back offline and compares every checksum.
		std::FILE* s_journal = nullptr;
		struct JPlan
		{
			Plan plan;
		};
		std::vector<JPlan> s_jplans;
		std::map<s32, u32> s_jsums; // global save id -> recorded checksum
		size_t s_jnext = 0;
		s32 s_save_seq = 0;                 // global save id counter
		std::map<s32, s32> s_save_to_bridge; // global save id -> the bridge's save index of the current plan
		u32 s_jcompares = 0, s_jmismatches = 0;
		s32 s_jfirst_mismatch = -1;

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
							s_lib.GetSymbol("pcb_get_stats", &p_get_stats) && s_lib.GetSymbol("pcb_last_error", &p_last_error) &&
							s_lib.GetSymbol("pcb_replay_open", &p_replay_open) && s_lib.GetSymbol("pcb_replay_close", &p_replay_close) &&
							s_lib.GetSymbol("pcb_replay_frame", &p_replay_frame) &&
							s_lib.GetSymbol("pcb_replay_resolve_save", &p_replay_resolve_save) &&
							s_lib.GetSymbol("pcb_replay_get_stats", &p_replay_get_stats);
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
		// Replay anchor for a token host: the anchor is the session's frame-0 save, i.e. the state the session started
		// from (the savestate every peer and every playback boots). The blob identifies it by the watched-state hash;
		// playback starts from the same savestate and import_state proves it is at that exact state.
		struct AnchorBlob
		{
			u32 magic;
			s32 frame;
			u32 checksum;
			char serial[20];
		};
		static constexpr u32 ANCHOR_MAGIC = 0x41425250; // "PRBA"
		u32 s_resolving_checksum = 0;                   // raw hash of the save being answered (export_state runs inside)

		uint32_t PCB_CALL CbExportState(void*, int32_t frame, uint8_t* dst, uint32_t cap)
		{
			if (!dst)
				return sizeof(AnchorBlob);
			if (cap < sizeof(AnchorBlob))
				return 0;
			AnchorBlob b = {};
			b.magic = ANCHOR_MAGIC;
			b.frame = frame;
			b.checksum = s_resolving_checksum;
			std::strncpy(b.serial, s_cfg.game_id.c_str(), sizeof(b.serial) - 1);
			std::memcpy(dst, &b, sizeof(b));
			return sizeof(b);
		}
		int32_t PCB_CALL CbImportState(void*, int32_t frame, const uint8_t* src, uint32_t len)
		{
			AnchorBlob b = {};
			if (len != sizeof(b))
				return 0;
			std::memcpy(&b, src, sizeof(b));
			const u32 now = s_host.state_checksum ? s_host.state_checksum() : 0;
			if (b.magic != ANCHOR_MAGIC || b.checksum != now)
			{
				Console.Error("NetBridge: replay anchor (frame %d, %s, state %08X) is not the current state %08X: start playback "
							  "from the recording's savestate",
					frame, b.serial, b.checksum, now);
				return 0;
			}
			Console.WriteLn("NetBridge: replay anchor verified (frame %d, state %08X)", frame, now);
			return 1;
		}

		void PCB_CALL CbDesync(void*, const pcb_desync_info* info)
		{
			Console.Error("NetBridge: DESYNC at frame %d (local %08X remote %08X kind %d)", info->frame, info->local_checksum,
				info->remote_checksum, info->kind);
			if (s_host.on_desync)
				s_host.on_desync(info->frame, info->local_checksum, info->remote_checksum, info->kind);
		}
	} // namespace

	bool LoadJournal(const std::string& path, std::string* error)
	{
		s_jplans.clear();
		s_jsums.clear();
		s_jnext = 0;
		std::FILE* f = std::fopen(path.c_str(), "r");
		if (!f)
		{
			if (error)
				*error = fmt::format("cannot open journal {}", path);
			return false;
		}
		char line[512];
		while (std::fgets(line, sizeof(line), f))
		{
			if (line[0] == 'P')
			{
				JPlan jp;
				int has_load = 0, load_frame = -1, rb = 0, n = 0;
				std::sscanf(line + 1, "%d %d %d %d", &n, &has_load, &load_frame, &rb);
				jp.plan.has_load = has_load != 0;
				jp.plan.load_frame = load_frame;
				jp.plan.rollback_advances = static_cast<u32>(rb);
				s_jplans.push_back(std::move(jp));
			}
			else if (line[0] == 'S' && !s_jplans.empty())
			{
				Step st;
				int frame = 0, rbk = 0, id = -1;
				unsigned b[12] = {};
				std::sscanf(line + 1, "%d %d %d %x %x %x %x %x %x %x %x %x %x %x %x", &frame, &rbk, &id, &b[0], &b[1], &b[2], &b[3],
					&b[4], &b[5], &b[6], &b[7], &b[8], &b[9], &b[10], &b[11]);
				st.frame = frame;
				st.rolling_back = rbk != 0;
				st.save_index = id;
				for (int k = 0; k < 12; k++)
					st.inputs[k / 6][k % 6] = static_cast<u8>(b[k]);
				s_jplans.back().plan.steps.push_back(st);
			}
			else if (line[0] == 'Q' && !s_jplans.empty())
			{
				int id = -1;
				std::sscanf(line + 1, "%d", &id);
				s_jplans.back().plan.pre_saves.push_back(id);
			}
			else if (line[0] == 'C')
			{
				int id = 0;
				unsigned sum = 0;
				std::sscanf(line + 1, "%d %x", &id, &sum);
				s_jsums[id] = sum;
			}
		}
		std::fclose(f);
		return !s_jplans.empty();
	}

	bool Start(const Config& cfg, Host host, std::string* error)
	{
		Stop();
		s_cfg = cfg;
		s_host = std::move(host);
		s_save_seq = 0;
		s_jcompares = s_jmismatches = 0;
		s_jfirst_mismatch = -1;
		if (cfg.mode == Mode::JournalReplay)
		{
			if (!LoadJournal(cfg.journal_path, error))
				return false;
			Console.WriteLn("NetBridge: journal replay of %s (%zu frames, %zu checksums)", cfg.journal_path.c_str(), s_jplans.size(),
				s_jsums.size());
			return true;
		}
		if (!Load(error))
			return false;
		if (!cfg.journal_path.empty())
		{
			s_journal = std::fopen(cfg.journal_path.c_str(), "w");
			if (s_journal)
				std::fprintf(s_journal, "# ps2rb host schedule journal: P plan, S step (frame rb saveid 12 input bytes), C checksum\n");
		}
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
		h.export_state = CbExportState;
		h.import_state = CbImportState;
		if (cfg.mode == Mode::Replay)
		{
			c.replay_path = nullptr;
			s_replay_h = p_replay_open(s_replay.c_str(), &c, &h);
			if (!s_replay_h)
			{
				if (error)
					*error = fmt::format("pcb_replay_open: {}", p_last_error());
				return false;
			}
			Console.WriteLn("NetBridge: playing %s", s_replay.c_str());
			return true;
		}
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
		if (s_replay_h)
		{
			pcb_replay_stats st = {};
			st.struct_size = sizeof(st);
			p_replay_get_stats(s_replay_h, &st);
			Console.WriteLn("NetBridge: replay stopped: %d/%d frames, checks %d ok %d failed (first %d)", st.frame, st.total_frames,
				st.checks_ok, st.checks_failed, st.first_fail_frame);
			p_replay_close(s_replay_h);
			s_replay_h = nullptr;
		}
		if (s_journal)
		{
			std::fclose(s_journal);
			s_journal = nullptr;
		}
		if (s_cfg.mode == Mode::JournalReplay && !s_jplans.empty())
		{
			Console.WriteLn("NetBridge: journal replay stopped: %zu/%zu frames, %u checksum compares, %u mismatches (first frame %d)",
				s_jnext, s_jplans.size(), s_jcompares, s_jmismatches, s_jfirst_mismatch);
			s_jplans.clear();
		}
	}

	bool Active() { return s_session != nullptr || s_replay_h != nullptr || !s_jplans.empty(); }

	int Frame(Plan* plan)
	{
		plan->has_load = false;
		plan->load_frame = -1;
		plan->rollback_advances = 0;
		plan->steps.clear();
		plan->pre_saves.clear();
		s_save_to_bridge.clear();
		if (s_cfg.mode == Mode::JournalReplay)
		{
			if (s_jnext >= s_jplans.size())
				return -1; // finished
			*plan = s_jplans[s_jnext++].plan;
			return static_cast<int>(plan->steps.size());
		}
		const pcb_plan* p = nullptr;
		int32_t n;
		if (s_replay_h)
			n = p_replay_frame(s_replay_h, &p) > 0 ? 1 : -1;
		else if (s_session)
			n = p_frame(s_session, &p);
		else
			return -1;
		if (n <= 0 || !p)
			return n;
		for (int32_t k = 0; k < p->event_count; k++)
		{
			const pcb_event& e = p->events[k];
			if (e.type == PCB_EV_LOAD)
			{
				plan->has_load = true;
				plan->load_frame = e.frame;
				if ((e.flags & PCB_EVF_ANCHOR_BLOB) && CbImportState(nullptr, e.frame, e.state, e.state_len) != 1)
					return -1; // playback not started at the recording's anchor state
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
			else if (e.type == PCB_EV_SAVE)
			{
				const s32 id = s_save_seq++;
				s_save_to_bridge[id] = e.save_index;
				if (plan->steps.empty())
					plan->pre_saves.push_back(id);
				else
					plan->steps.back().save_index = id;
			}
		}
		if (s_replay_h)
			n = static_cast<int32_t>(plan->steps.size());
		if (s_journal)
		{
			std::fprintf(s_journal, "P %zu %d %d %u\n", plan->steps.size(), plan->has_load ? 1 : 0, plan->load_frame,
				plan->rollback_advances);
			for (const s32 id : plan->pre_saves)
				std::fprintf(s_journal, "Q %d\n", id);
			for (const Step& st : plan->steps)
			{
				std::fprintf(s_journal, "S %d %d %d", st.frame, st.rolling_back ? 1 : 0, st.save_index);
				for (int k = 0; k < 12; k++)
					std::fprintf(s_journal, " %02x", st.inputs[k / 6][k % 6]);
				std::fprintf(s_journal, "\n");
			}
		}
		return n;
	}

	void ResolveSave(s32 save_id, u32 checksum)
	{
		if (save_id < 0)
			return;
		s_resolving_checksum = checksum;
		checksum = pcb_finalize_checksum(checksum);
		if (s_journal)
			std::fprintf(s_journal, "C %d %08x\n", save_id, checksum);
		if (s_cfg.mode == Mode::JournalReplay)
		{
			const auto it = s_jsums.find(save_id);
			if (it != s_jsums.end())
			{
				s_jcompares++;
				if (it->second != checksum)
				{
					if (s_jmismatches++ == 0)
					{
						s_jfirst_mismatch = static_cast<s32>(s_jnext) - 1;
						Console.Error("NetBridge: journal replay MISMATCH at save %d (plan %zu): %08X vs recorded %08X", save_id, s_jnext - 1,
							checksum, it->second);
					}
				}
			}
			return;
		}
		const auto b = s_save_to_bridge.find(save_id);
		if (b == s_save_to_bridge.end())
			return;
		if (s_session)
			p_resolve_save(s_session, b->second, checksum, 0);
		else if (s_replay_h)
			p_replay_resolve_save(s_replay_h, b->second, checksum, 0);
	}

	float PaceFactor()
	{
		if (!s_session)
			return 1.0f;
		pcb_stats st = {};
		st.struct_size = sizeof(st);
		p_get_stats(s_session, &st);
		return st.pace_factor > 0.5f ? st.pace_factor : 1.0f;
	}

	std::string Status()
	{
		if (s_cfg.mode == Mode::JournalReplay && !s_jplans.empty())
			return fmt::format("net: journal replay {}/{} frames, {} compares, {} mismatches (first {})", s_jnext, s_jplans.size(),
				s_jcompares, s_jmismatches, s_jfirst_mismatch);
		if (s_replay_h)
		{
			pcb_replay_stats st = {};
			st.struct_size = sizeof(st);
			p_replay_get_stats(s_replay_h, &st);
			return fmt::format("net: replay {}/{} frames, checks {} ok {} failed (first {}) finished {}", st.frame, st.total_frames,
				st.checks_ok, st.checks_failed, st.first_fail_frame, st.finished);
		}
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
