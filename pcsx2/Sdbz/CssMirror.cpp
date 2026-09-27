// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "CssMirror.h"

#include "Memory.h"
#include "common/Console.h"

#include "fmt/format.h"

#include <algorithm>
#include <cstring>
#include <random>

namespace CssMirror
{
	namespace
	{
		constexpr u32 RAM_MASK = 0x01FFFFFFu;
		enum Phase : u8
		{
			BUSY = 0,
			BROWSE = 1,
			COLOUR = 2,
			LOCKED = 3,
		};

#pragma pack(push, 1)
		// one side's state as its owner sees it; re-sent whenever it changes (the link channel is reliable and ordered,
		// so the newest record is always complete)
		struct Rec
		{
			u8 ver = 1;
			u8 side = 0;
			u16 visit = 0;       // character-select visit (scene edges into the CSS since the session start)
			u8 phase = BUSY;     // the stable phase the side is in or heading to
			u8 col = 0, row = 0; // cursor cell
			s8 chr = -1, colour = 0; // char / colour for COLOUR and LOCKED
			u8 stage_loop = 0;   // side 0 only: the stage-select read loop is live
			s8 stage = 0;        // stage cursor (0 = RANDOM)
			s8 bgm = 0;          // BGM index
			s8 stage_final = -1; // stage confirmed in this stage-select entry (resolved id)
			u8 stage_entry = 0;  // stage-select entries this visit
			u8 cancel_entry = 0; // the entry the owner cancelled (0 = none)
			u8 pad = 0;
			u32 lock_id = 0;     // != 0: this side is locking / locked (or holds a lock intent)
			u32 echo = 0;        // the other side's lock_id this side has committed to
		};
#pragma pack(pop)

		struct Pulse
		{
			u32 frame = 0xFFFFFFFFu;
			u16 btn = 0;
		};

		Config s_cfg;
		int s_local = 0;
		std::mt19937 s_rng;
		u32 s_frame = 0;
		u32 s_obj = 0;          // the CSS script object (VM - 8)
		u32 s_obj_frame = 0;    // frame the object last ticked
		u32 s_prev_scene = 0xFFFFFFFFu;
		u16 s_visit = 0;
		bool s_in_css = false;
		Rec s_mine, s_sent, s_remote;
		bool s_have_remote = false;
		u32 s_lock_ctr = 0;
		bool s_intent = false;      // our final lock is held until the other side commits
		bool s_fire_ok = false;     // release the held lock
		u16 s_prev_local = 0;
		std::array<Pulse, 2> s_pulse{};
		bool s_auto = false;        // owner-side RANDOM: driving our own side to a concrete pick
		s8 s_auto_chr = -1, s_auto_colour = 0;

		u32 R32(u32 a)
		{
			u32 v;
			std::memcpy(&v, &eeMem->Main[a & RAM_MASK & ~3u], 4);
			return v;
		}
		s32 S32(u32 a) { return static_cast<s32>(R32(a)); }
		bool Bit(u32 base, u32 n) { return (eeMem->Main[(base + (n >> 3)) & RAM_MASK] >> (n & 7)) & 1; }

		u32 Vars() { return s_obj ? R32(R32(s_obj + 0x28)) : 0; }
		s32 Var(u32 i)
		{
			const u32 v = Vars();
			return v ? S32(v + 4 * i) : 0;
		}
		u32 T0() { return s_obj ? R32(s_obj + 0x14) : 0; }
		u32 Base() { return T0() ? R32(T0() + 92) : 0; }
		// script-relative pc of VM thread n (0xFFFFFFFF = not running)
		u32 Pc(u32 n)
		{
			const u32 t0 = T0();
			if (!t0)
				return 0xFFFFFFFFu;
			const u32 pc = R32(t0 + 96 * n + 88);
			return pc ? pc - Base() : 0xFFFFFFFFu;
		}
		s32 Locked(int s) { return S32(s_cfg.side_rec[s] + 4); }
		s32 LockedColour(int s) { return S32(s_cfg.side_rec[s] + 8); }
		// grid: the move/selectable table is always the first one; confirm and display use the second once unlocked
		s32 GridMove(u32 k) { return S32(Base() + s_cfg.grid_table + 4 * k); }
		s32 GridShow(u32 k) { return S32(Base() + (Var(s_cfg.var_diarmuid) == 1 ? s_cfg.grid_table_b : s_cfg.grid_table) + 4 * k); }
		bool Selectable(s32 c) { return c == 0 || (c > 0 && Bit(s_cfg.unlock_bits, s_cfg.unlock_base + static_cast<u32>(c))); }
		u32 ColourCount(s32 c)
		{
			if (c == 0 || c == 14 || c == 15)
				return 2;
			if (c >= 1 && c <= 13)
				return static_cast<u32>(std::max(1, Var(s_cfg.var_count_a + static_cast<u32>(c - 1))));
			if (c >= 16 && c <= 19)
				return static_cast<u32>(std::max(1, Var(s_cfg.var_count_b + static_cast<u32>(c - 16))));
			return 1;
		}
		void WVar(u32 i, s32 v)
		{
			if (const u32 t = Vars())
				std::memcpy(&eeMem->Main[(t + 4 * i) & RAM_MASK], &v, 4);
		}
		// stage select is live: both sides locked on this PC and the stage not confirmed yet (memory, not thread pcs)
		bool StageSelect()
		{
			return Locked(0) != -1 && Locked(1) != -1 && R32(s_cfg.side_rec[0]) != s_cfg.confirmed_value;
		}
		bool StageAvail(s32 k) { return k == 0 || Var(s_cfg.var_stage_avail + static_cast<u32>(k)) == 1; }

		Phase SidePhase(int s)
		{
			const u32 pc = Pc(s_cfg.side_thread[s]);
			if (pc == s_cfg.pc_browse[s])
				return BROWSE;
			if (pc == s_cfg.pc_colour[s])
				return COLOUR;
			if (pc == s_cfg.pc_locked[s] && Locked(s) != -1)
				return LOCKED;
			return BUSY;
		}

		// the owner's view of its side: the phase it is in or heading to (confirm waits report their destination)
		void ReadSide(int s, Rec& r)
		{
			r.side = static_cast<u8>(s);
			r.visit = s_visit;
			r.col = static_cast<u8>(Var(s_cfg.var_col[s]));
			r.row = static_cast<u8>(Var(s_cfg.var_row[s]));
			const u32 pc = Pc(s_cfg.side_thread[s]);
			const s32 chr = Var(s_cfg.var_chr[s]);
			if (Locked(s) != -1)
			{
				r.phase = LOCKED;
				r.chr = static_cast<s8>(Locked(s));
				r.colour = static_cast<s8>(LockedColour(s));
			}
			else if (pc == s_cfg.pc_colour_confirm[s])
			{
				r.phase = LOCKED;
				r.chr = static_cast<s8>(chr);
				r.colour = static_cast<s8>(Var(s_cfg.var_colour[s]));
			}
			else if (chr != -1)
			{
				r.phase = COLOUR;
				r.chr = static_cast<s8>(chr);
				r.colour = static_cast<s8>(Var(s_cfg.var_colour[s]));
			}
			else if (pc == s_cfg.pc_confirm[s])
			{
				r.phase = COLOUR;
				r.chr = static_cast<s8>(GridShow(r.row * s_cfg.grid_cols + r.col));
				r.colour = 0;
			}
			else
			{
				r.phase = BROWSE;
				r.chr = -1;
				r.colour = 0;
			}
		}

		// one step of the cursor from (col,row) toward (tc,tr) with the game's own move rules
		u32 StepR(u32 col, u32 row)
		{
			u32 c = col;
			for (u32 i = 0; i < s_cfg.grid_cols; i++)
			{
				c = (c + 1) % s_cfg.grid_cols;
				if (Selectable(GridMove(row * s_cfg.grid_cols + c)))
					return c;
			}
			return col;
		}
		u32 StepL(u32 col, u32 row)
		{
			u32 c = col;
			for (u32 i = 0; i < s_cfg.grid_cols; i++)
			{
				c = (c + s_cfg.grid_cols - 1) % s_cfg.grid_cols;
				if (Selectable(GridMove(row * s_cfg.grid_cols + c)))
					return c;
			}
			return col;
		}
		u16 CursorStep(u32 col, u32 row, u32 tc, u32 tr)
		{
			if (row != tr)
			{
				const bool vertical_ok = (col != s_cfg.random_col || Var(s_cfg.var_diarmuid) == 1) &&
										 Selectable(GridMove(tr * s_cfg.grid_cols + col));
				if (vertical_ok)
					return s_cfg.btn_down; // 2 rows: a toggle
				return s_cfg.btn_right;    // move to a column where the row change is legal
			}
			u32 r = 0, l = 0, c = col;
			for (; c != tc && r <= s_cfg.grid_cols; r++)
				c = StepR(c, row);
			c = col;
			for (; c != tc && l <= s_cfg.grid_cols; l++)
				c = StepL(c, row);
			return (r <= l) ? s_cfg.btn_right : s_cfg.btn_left;
		}
		// the cell showing char c (prefer the owner's own cell when it shows c)
		bool CellOf(s32 c, u32 pref_col, u32 pref_row, u32* col, u32* row)
		{
			const u32 pk = pref_row * s_cfg.grid_cols + pref_col;
			if (pk < s_cfg.grid_cols * s_cfg.grid_rows && GridShow(pk) == c)
			{
				*col = pref_col, *row = pref_row;
				return true;
			}
			for (u32 k = 0; k < s_cfg.grid_cols * s_cfg.grid_rows; k++)
				if (GridShow(k) == c)
				{
					*col = k % s_cfg.grid_cols, *row = k / s_cfg.grid_cols;
					return true;
				}
			return false;
		}

		// drive side s (on this PC) toward the target record; returns the button to press now (0 = nothing)
		u16 DriveSide(int s, const Rec& t, u32 my_lock_id)
		{
			const Phase p = SidePhase(s);
			if (p == BUSY)
				return 0;
			const u32 col = static_cast<u32>(Var(s_cfg.var_col[s])), row = static_cast<u32>(Var(s_cfg.var_row[s]));
			if (p == BROWSE)
			{
				if (t.phase == BROWSE)
					return (col != t.col || row != t.row) ? CursorStep(col, row, t.col, t.row) : 0;
				if (t.phase == COLOUR || t.phase == LOCKED)
				{
					u32 tc, tr;
					if (!CellOf(t.chr, t.col, t.row, &tc, &tr))
						return 0;
					return (col != tc || row != tr) ? CursorStep(col, row, tc, tr) : s_cfg.btn_ok;
				}
				return 0;
			}
			if (p == COLOUR)
			{
				const s32 chr = Var(s_cfg.var_chr[s]);
				if (t.phase == BROWSE || ((t.phase == COLOUR || t.phase == LOCKED) && t.chr != chr))
					return s_cfg.btn_back;
				const s32 colour = Var(s_cfg.var_colour[s]);
				// a colour press that changed nothing (a skip rule the patch did not cover): place the colour directly
				static s32 s_last_colour[2] = {-1, -1};
				static u32 s_same[2] = {0, 0};
				if (t.colour != colour)
				{
					s_same[s] = (colour == s_last_colour[s]) ? s_same[s] + 1 : 0;
					s_last_colour[s] = colour;
					if (s_same[s] >= 8)
					{
						WVar(s_cfg.var_colour[s], t.colour);
						s_same[s] = 0;
						return 0;
					}
					const u32 n = ColourCount(chr);
					const u32 fwd = (static_cast<u32>(t.colour) + n - static_cast<u32>(colour)) % n;
					return (fwd <= n - fwd) ? s_cfg.btn_right : s_cfg.btn_left;
				}
				if (t.phase == LOCKED)
				{
					// the lock that would make both sides locked here waits for the other PC's commitment
					if (my_lock_id != 0 && t.echo != my_lock_id)
						return 0;
					return s_cfg.btn_ok;
				}
				return 0;
			}
			// LOCKED
			if (t.phase != LOCKED || t.chr != Locked(s) || t.colour != LockedColour(s))
				return s_cfg.btn_back; // the owner un-picked (the game allows it only while the other side is not locked)
			return 0;
		}

		// P1 owns the stage: on P2's PC drive port 0 to P1's confirmed stage, then confirm. Presses that land outside the
		// stage read loop (the transition waits) are simply not read; the pulse repeats until the confirm lands.
		u16 DriveStage(const Rec& t)
		{
			if (t.phase != LOCKED)
				return s_cfg.btn_back; // P1 cancelled out of stage select (both picks reset on P1's PC): same here
			if (t.stage_final <= 0)
				return 0;
			const s32 want = t.stage_final, cur = Var(s_cfg.var_stage);
			if (cur != want)
			{
				if (!StageAvail(want))
				{
					WVar(s_cfg.var_stage, want); // unreachable by the cursor (RANDOM quirk): place it directly
					return 0;
				}
				const s32 n = static_cast<s32>(s_cfg.stage_entries) + 1;
				s32 r = 0, l = 0, c = cur;
				for (; c != want && r <= n; r++)
					do
						c = (c + 1) % n;
					while (!StageAvail(c));
				c = cur;
				for (; c != want && l <= n; l++)
					do
						c = (c + n - 1) % n;
					while (!StageAvail(c));
				return (r <= l) ? s_cfg.btn_right : s_cfg.btn_left;
			}
			if (Var(s_cfg.var_bgm_ok) && t.bgm != Var(s_cfg.var_bgm))
			{
				const s32 n = static_cast<s32>(s_cfg.bgm_count);
				const s32 fwd = (t.bgm - Var(s_cfg.var_bgm) + n) % n;
				return (fwd <= n - fwd) ? s_cfg.btn_r1 : s_cfg.btn_l1;
			}
			return s_cfg.btn_ok;
		}

		// one press per two frames on a port: press this frame, release the next, then re-evaluate from fresh state
		u16 PulseOut(u32 port, u16 want)
		{
			Pulse& p = s_pulse[port & 1];
			if (s_frame == p.frame)
				return p.btn;
			if (s_frame == p.frame + 1)
				return 0;
			if (!want)
				return 0;
			p.frame = s_frame;
			p.btn = want;
			return want;
		}
		bool PulseIdle(u32 port) { return s_frame > s_pulse[port & 1].frame + 1 || s_pulse[port & 1].frame == 0xFFFFFFFFu; }

		const char* PhaseName(u8 p)
		{
			static const char* n[] = {"busy", "browse", "colour", "locked"};
			return p < 4 ? n[p] : "?";
		}

		void RefreshLatch()
		{
			const u32 scene = R32(s_cfg.scene_addr);
			if (scene == s_cfg.scene_value && s_prev_scene != s_cfg.scene_value)
			{
				s_visit++;
				s_intent = s_fire_ok = s_auto = false;
				s_lock_ctr += 1;
				s_mine.lock_id = s_mine.echo = 0;
				s_mine.stage_final = -1;
			}
			s_prev_scene = scene;
			if (scene != s_cfg.scene_value)
				s_obj = 0;
			s_in_css = s_obj != 0 && scene == s_cfg.scene_value && Vars() != 0 && s_frame <= s_obj_frame + 2;
		}
	} // namespace

	void Configure(const Config& cfg) { s_cfg = cfg; }
	bool Enabled() { return s_cfg.enabled; }
	u32 TickHookPc() { return s_cfg.enabled ? s_cfg.tick_hook : 0; }
	bool InCss() { return s_cfg.enabled && s_in_css; }

	void Reset(int local_side, u32 seed)
	{
		s_local = local_side & 1;
		s_rng.seed(seed ^ (0x9E3779B9u * static_cast<u32>(local_side + 1)));
		s_obj = 0;
		s_prev_scene = 0xFFFFFFFFu;
		s_visit = 0;
		s_in_css = false;
		s_mine = s_sent = s_remote = Rec{};
		s_sent.ver = 0; // force the first send
		s_have_remote = false;
		s_lock_ctr = 0;
		s_intent = s_fire_ok = s_auto = false;
		s_prev_local = 0;
		s_pulse = {};
	}

	void OnScriptTick(u32 vm)
	{
		if (!s_cfg.enabled || R32(s_cfg.scene_addr) != s_cfg.scene_value)
			return;
		const u32 t0 = R32(vm + 12);
		const u32 base = t0 ? R32(t0 + 92) : 0;
		if (!base || s_cfg.sig.empty())
			return;
		if (std::memcmp(&eeMem->Main[(base + s_cfg.sig_off) & RAM_MASK], s_cfg.sig.data(), s_cfg.sig.size()) != 0)
			return;
		if (s_obj != vm - 8)
			Console.WriteLn("CssMirror: character select latched (object %08x, visit %u)", vm - 8, s_visit);
		s_obj = vm - 8;
		s_obj_frame = s_frame;
	}

	void OnMessage(const u8* data, u32 len)
	{
		if (len < sizeof(Rec))
			return;
		Rec r;
		std::memcpy(&r, data, sizeof(Rec));
		if (r.ver != 1 || r.side == s_local)
			return;
		s_remote = r;
		s_have_remote = true;
	}

	void Frame(u32 menu_frame, const std::function<void(const void*, u32)>& send)
	{
		if (!s_cfg.enabled)
			return;
		s_frame = menu_frame;
		RefreshLatch();
		if (!s_in_css)
			return;
		const int x = s_local, r = s_local ^ 1;
		const u32 keep_id = s_mine.lock_id, keep_echo = s_mine.echo;
		ReadSide(x, s_mine);
		s_mine.lock_id = keep_id;
		s_mine.echo = keep_echo;

		// our lock id lives while we are locking/locked or hold an intent; back in the grid it is gone
		if (s_mine.phase == BROWSE && !s_intent && !s_auto)
			s_mine.lock_id = 0;
		if (s_mine.phase == COLOUR && !s_intent && Locked(x) == -1 && SidePhase(x) == COLOUR && s_mine.lock_id != 0 &&
			Pc(s_cfg.side_thread[x]) != s_cfg.pc_colour_confirm[x])
			s_mine.lock_id = 0; // a lock we let through was cancelled before it landed
		// commitment: we are locking/locked and the other side is too -> we commit to its lock (our × is then blocked)
		const bool remote_live = s_have_remote && s_remote.visit == s_visit;
		s_mine.echo = (s_mine.lock_id != 0 && remote_live && s_remote.lock_id != 0) ? s_remote.lock_id : 0;

		// stage select (P1 owns it): its record carries the live cursor/BGM and, once confirmed, the stage id
		if (x == 0)
		{
			s_mine.stage_loop = StageSelect() ? 1 : 0;
			s_mine.stage = static_cast<s8>(Var(s_cfg.var_stage));
			s_mine.bgm = static_cast<s8>(Var(s_cfg.var_bgm));
			s_mine.stage_final = (R32(s_cfg.side_rec[0]) == s_cfg.confirmed_value && s_cfg.stage_id_addr) ?
									 static_cast<s8>(R32(s_cfg.stage_id_addr)) :
									 static_cast<s8>(-1);
		}

		static u8 s_log_mine = 0xFF, s_log_remote = 0xFF;
		if (s_mine.phase != s_log_mine || (remote_live && s_remote.phase != s_log_remote))
		{
			Console.WriteLn("CssMirror: visit %u f%u  mine P%d %s chr %d col %d lock %x echo %x | remote P%d %s (here %s) chr %d col %d lock %x echo %x",
				s_visit, s_frame, x + 1, PhaseName(s_mine.phase), s_mine.chr, s_mine.colour, s_mine.lock_id, s_mine.echo, r + 1,
				PhaseName(remote_live ? s_remote.phase : BUSY), PhaseName(SidePhase(r)), s_remote.chr, s_remote.colour, s_remote.lock_id,
				s_remote.echo);
			s_log_mine = s_mine.phase;
			s_log_remote = remote_live ? s_remote.phase : 0xFF;
		}
		if (std::memcmp(&s_mine, &s_sent, sizeof(Rec)) != 0)
		{
			send(&s_mine, sizeof(Rec));
			s_sent = s_mine;
		}
		(void)r;
	}

	u16 Pad(u32 port, u16 buttons)
	{
		if (!s_cfg.enabled)
			return buttons;
		const int x = s_local, r = s_local ^ 1;
		const bool remote_live = s_have_remote && s_remote.visit == s_visit;
		if (static_cast<int>(port) != x)
		{
			// the remote player's port: driven from their record (neutral outside the CSS or without a record)
			if (!s_in_css || !remote_live)
				return PulseOut(port, 0);
			u16 want = 0;
			if (r == 0 && StageSelect())
				want = DriveStage(s_remote);
			else if (Pc(s_cfg.side_thread[r]) != 0xFFFFFFFFu)
				want = DriveSide(r, s_remote, s_mine.lock_id);
			return PulseOut(port, want);
		}
		if (!s_in_css)
			return buttons;
		const u16 pressed = static_cast<u16>(buttons & ~s_prev_local);
		s_prev_local = buttons;
		const Phase p = SidePhase(x);

		// stage select (P1 owns it): × there cancels both picks on both PCs; remember which entry it cancelled
		if (StageSelect())
			return buttons; // P1's own stage select (× there cancels both picks; the record shows it)
		// owner-side RANDOM: drive our own side to a concrete pick drawn here (the pick travels as a normal one)
		if (s_auto)
		{
			Rec t = s_mine;
			t.chr = s_auto_chr;
			t.colour = s_auto_colour;
			t.phase = LOCKED;
			if (Locked(x) != -1 || (p == BROWSE && Var(s_cfg.var_chr[x]) == -1 && s_auto_chr < 0))
				s_auto = false;
			else if (p == COLOUR && t.colour == Var(s_cfg.var_colour[x]))
			{
				// the final ○ goes through the lock rules below
				s_auto = false;
				buttons = s_cfg.btn_ok;
				s_prev_local = 0;
				return Pad(port, buttons);
			}
			else
				return PulseOut(port, DriveSide(x, t, 0));
		}
		switch (p)
		{
			case BROWSE:
			{
				buttons &= static_cast<u16>(~s_cfg.btn_back); // × with nothing picked would leave to the title
				const u32 col = static_cast<u32>(Var(s_cfg.var_col[x])), row = static_cast<u32>(Var(s_cfg.var_row[x]));
				if ((pressed & s_cfg.btn_ok) && GridShow(row * s_cfg.grid_cols + col) == 0)
				{
					std::vector<s32> pool;
					for (u32 k = 0; k < s_cfg.grid_cols * s_cfg.grid_rows; k++)
					{
						const s32 c = GridShow(k);
						if (c > 0 && Selectable(c) && std::find(pool.begin(), pool.end(), c) == pool.end())
							pool.push_back(c);
					}
					if (!pool.empty())
					{
						s_auto_chr = static_cast<s8>(pool[s_rng() % pool.size()]);
						s_auto_colour = static_cast<s8>(s_rng() % ColourCount(s_auto_chr));
						s_auto = true;
						Console.WriteLn("CssMirror: RANDOM -> char %d colour %d", s_auto_chr, s_auto_colour);
					}
					buttons &= static_cast<u16>(~s_cfg.btn_ok);
				}
				return buttons;
			}
			case COLOUR:
			{
				if (s_intent)
				{
					buttons &= static_cast<u16>(~s_cfg.btn_ok);
					if (pressed & s_cfg.btn_back)
					{
						s_intent = false; // backed out while waiting
						s_mine.lock_id = 0;
						return buttons;
					}
					const bool remote_locked_here = Locked(r) != -1;
					if (!remote_locked_here || (remote_live && s_remote.echo == s_mine.lock_id))
						s_fire_ok = true;
					if (s_fire_ok && PulseIdle(port))
					{
						s_intent = s_fire_ok = false;
						return PulseOut(port, s_cfg.btn_ok);
					}
					return PulseOut(port, 0) | buttons;
				}
				if (pressed & s_cfg.btn_ok)
				{
					s_mine.lock_id = ++s_lock_ctr + (static_cast<u32>(x) << 31);
					if (Locked(r) != -1)
					{
						// our lock would make both sides locked right now: hold it until the other PC commits to it
						s_intent = true;
						buttons &= static_cast<u16>(~s_cfg.btn_ok);
					}
				}
				return buttons;
			}
			case LOCKED:
				if (s_mine.lock_id != 0 && s_mine.echo != 0 && remote_live && s_mine.echo == s_remote.lock_id)
					buttons &= static_cast<u16>(~s_cfg.btn_back); // committed: the other PC may already be past both-locked
				return buttons;
			default:
				return buttons;
		}
	}

	std::string Status()
	{
		if (!s_cfg.enabled)
			return "css mirror: off";
		return fmt::format("css mirror: visit {} {} mine ph {} cell {},{} chr {} col {} lock {:x} echo {:x} | remote ph {} "
						   "cell {},{} chr {} col {} lock {:x} echo {:x}{}{}",
			s_visit, s_in_css ? "live" : "idle", s_mine.phase, s_mine.col, s_mine.row, s_mine.chr, s_mine.colour, s_mine.lock_id,
			s_mine.echo, s_remote.phase, s_remote.col, s_remote.row, s_remote.chr, s_remote.colour, s_remote.lock_id,
			s_remote.echo, s_intent ? " INTENT" : "", s_auto ? " AUTO" : "");
	}
} // namespace CssMirror
