-- SDBZ training tools: HP / AC / SP locks, per-axis position hold + facing lock, and the whole-battle
-- sim-freeze / framestep. Plus the "Training" and "Framestep" control panes. init(ctx) wires cfg / pane;
-- player + battle helpers come from sdbz.players. Runtime state here is NOT persisted.
local P = require("sdbz.players")
local valid = P.valid

local M = {}
local cfg, pane

function M.init(ctx)
	cfg, pane = ctx.cfg, ctx.pane
end

-- ============================ training (runtime state) ============================
-- gage objects g_PlayerGage[0..1] (sdbz.players M.GAGE; each gage's +4 = OWNER player ptr). Per-gage field offsets:
-- white HP @+0x0C (CPlayerGage_CalcHPFrac = *(g+12)/250), AC @+0x10 (f32), SP/Ki @+0x1C (int), recoverable/red
-- "chip" @+0x30 (CalcChipFrac = *(g+48)/250) -- draining HP must also write the chip or the red bar lags.
-- gage[0]@0x5B1A10 OWNS P1 (0x5ADFB0), gage[1]@0x5B1A48 OWNS P2 (0x5AFC80) -- SAME order as the player array
-- (an earlier comment claimed it was reversed -> HP/AC/SP edited the wrong player; that was screen-side confusion).
-- We now bind the gage to each player by P.gage_for(idx) (the +4 owner pointer), NOT a hardcoded order.
local G_HP, G_HP_RED, G_AC, G_SP = 0x0C, 0x30, 0x10, 0x1C
-- CPlayerHitTimer (memory sdbz-stun-collision): POINTER @player+6212; 4 sub-timers, each countdown VALUE at
-- hitTimer+8/+16/+24/+32. Ticked down 1/frame by CPlayerHitTimer_TickAll@0x1D2F50 from CPlayerBase__m06 while in
-- an active reaction. Each stun TYPE (hitstun / blockstun / dizzy) is one of these four -- the readout below is
-- ALSO the live which-is-which map (hit / block / dizzy the dummy, watch which value jumps). Values are 16-bit
-- range (HitTimer_Set takes __int16); hold well under 0x7FFF. "Infinite stun" = while any is >0 (the game set a
-- stun), re-write it high each frame so it never counts to 0 (the reaction recovers only at 0).
local HITTIMER_PTR = 6212
local STUN_OFFS = { 8, 16, 24, 32 }
local STUN_HOLD = 3000 -- ~50s; well within s16 so the engine's own reads stay sane
local function hit_timer(pp) -- returns the CPlayerHitTimer object addr, or nil
	if not P.player_valid(pp) then return nil end
	local ht = memory.read_u32(pp + HITTIMER_PTR)
	if not valid(ht) then return nil end
	return ht
end
local train = {
	-- per-player locks so P1 and P2 are independent (one "Lock" per player, not a shared one)
	lock_hp = { false, false }, lock_ac = { false, false }, lock_sp = { false, false },
	inf_sp = { false, false }, inf_ac = { false, false }, -- engine-native infinite-meter flags (gage+8)
	inf_stun = { false, false }, -- hold the CPlayerHitTimer sub-timers (infinite hitstun/blockstun/dizzy)
	hp = { 250, 250 }, ac = { 0.0, 0.0 }, sp = { 0, 0 },
	lock_pos = { { false, false, false }, { false, false, false } }, -- per-player, per-axis {X,Y,Z} hold
	lock_rot = { false, false }, -- facing lock -> drives the facing_patches() code patch (no per-frame data hold)
	pos = { { 0, 0, 0 }, { 0, 0, 0 } },
}

-- PER-PLAYER "Lock Facing": TWO detour hooks (one per facing path), each suppressing only the LOCKED player's
-- side -- the cave checks a1 == P1 0x5ADFB0 / P2 0x5AFC80 against per-player lock bytes @0x43C780(P1)/0x43C781(P2)
-- the script writes each frame; the unlocked side runs the real code untouched. So locking the DUMMY's F freezes
-- ONLY its facing while you play 100% normally.
--  1. GATE @0x1E3970 (PlayerState_TurnAround_CmdCheck_AngleGate) -> cave1 @0x43C700: the DISCRETE cross-up auto-
--     turn-around (state 52). Locked -> return -1 (decline); else the real gate. Gates on $a1 (a STATE handler:
--     called as (stateObj=$a0, player=$a1), so the player ptr is in $a1).
--  2. FACE @0x1D1A80 (Player_FaceOpponentYaw) -> cave2 @0x43C880: the per-frame CONTINUOUS face-tracking + the
--     cross-up RE-FACE on getting hit (the reaction-state Enter calls Player_FaceOpponent_CrossupCorrect@0x302C80
--     -> FaceOpponentYaw). Locked -> jr $ra (no rotation); else the real applier. Gates on $a0 -- FaceOpponentYaw
--     is called as (player=$a0, dirvec=$a1), so the player ptr is in $a0, NOT $a1. (Original cave2 copy-pasted the
--     $a1 compare from cave1 -> never matched -> always fell through -> facing was never actually locked while
--     moving or after hitstun. Fixed to $a0 = 0x1088xxxx beq.) Relocated 0x43C800->0x43C880 (a fresh, never-
--     executed slot in the gap) so the corrected body translates fresh on the next detour without an emulator
--     reset -- the old 0x43C800 block is already recompiler-cached this session (FaceOpponentYaw runs every frame).
-- Caves = hand-verified MIPS in a verified all-zero/unused gap (0x43C700..0x43CA00, between two unreferenced jump
-- tables). The cave BODIES are written via memory.write_u32 (not engine.patch) so they cost ZERO recompiler patch
-- slots -- the gap is never executed until a detour jumps in, so it translates fresh from RAM. Only the 4 entry
-- words use engine.patch, so the host reverts the detours on reload (UnpatchAll); the cave bytes then just linger
-- unreachable. RE'd live: gate = state-52 cancel; face = player+64 writer. See memory sdbz-lock-facing.
local GATE_FN, CAVE1, J_TO_CAVE1 = 0x1E3970, 0x43C700, 0x0810F1C0
local FACE_FN, CAVE2, J_TO_CAVE2 = 0x1D1A80, 0x43C880, 0x0810F220
local LOCK_BYTE = { 0x43C780, 0x43C781 } -- per-player lock flag the caves read (P1, P2)
local CAVE1_WORDS = { -- gate detour @0x43C700: LOCKED -> li $v0,-1; jr $ra | else orig prologue (addiu sp,-0xA0; sd ra,0x10) + j 0x1E3978
	0x3C090043, 0x3529C780, 0x3C08005A, 0x3508DFB0, 0x10A80007, 0x00000000,
	0x3C08005A, 0x3508FC80, 0x10A80008, 0x00000000, 0x0810F1D4, 0x00000000,
	0x912A0000, 0x1540000A, 0x00000000, 0x0810F1D4, 0x00000000,
	0x912A0001, 0x15400005, 0x00000000,
	0x27BDFF60, 0xFFBF0010, 0x08078E5E, 0x00000000,
	0x2402FFFF, 0x03E00008, 0x00000000,
}
local CAVE2_WORDS = { -- face detour @0x43C880: beq on $a0 (player ptr); LOCKED -> jr $ra (no rotation) | else orig prologue (addiu sp,-0x80; sd ra,0x50) + j 0x1D1A88; internal "run original" j -> 0x43C8D0 (base+0x50)
	0x3C090043, 0x3529C780, 0x3C08005A, 0x3508DFB0, 0x10880007, 0x00000000,
	0x3C08005A, 0x3508FC80, 0x10880008, 0x00000000, 0x0810F234, 0x00000000,
	0x912A0000, 0x1540000A, 0x00000000, 0x0810F234, 0x00000000,
	0x912A0001, 0x15400005, 0x00000000,
	0x27BDFF80, 0xFFBF0050, 0x080746A2, 0x00000000,
	0x03E00008, 0x00000000,
}
local facing_hooked = false
local function facing_hook(on)
	if on == facing_hooked then return end
	facing_hooked = on
	if on then
		for i, w in ipairs(CAVE1_WORDS) do memory.write_u32(CAVE1 + 4 * (i - 1), w) end -- write cave bodies (data; no patch slot)
		for i, w in ipairs(CAVE2_WORDS) do memory.write_u32(CAVE2 + 4 * (i - 1), w) end
		engine.patch(GATE_FN, J_TO_CAVE1); engine.patch(GATE_FN + 4, 0x00000000)         -- then detour both entries
		engine.patch(FACE_FN, J_TO_CAVE2); engine.patch(FACE_FN + 4, 0x00000000)
	else
		engine.unpatch(GATE_FN); engine.unpatch(GATE_FN + 4)                             -- remove the detours (caves linger, unreachable)
		engine.unpatch(FACE_FN); engine.unpatch(FACE_FN + 4)
	end
end
-- install the hooks while either F is on; refresh the per-player lock bytes each frame (the caves read them in-sim).
local function facing_patches(on)
	facing_hook(on)
	if on then
		-- LOCK_BYTE is per ARRAY slot (the caves compare a1 to the fixed addresses 0x5ADFB0/0x5AFC80). Map each
		-- array slot to its CURRENT logical side so "lock P1" follows the on-screen P1 even after a per-match swap.
		for ai = 0, 1 do
			memory.write_u8(LOCK_BYTE[ai + 1], train.lock_rot[P.logical_of(ai) + 1] and 1 or 0)
		end
	end
end

-- "hold" = write the locked stats / positions every frame
function M.training_update()
	if not P.in_battle() then facing_patches(false); return end -- battle-only; drop the facing patch in CSS/menus
	for p = 1, 2 do
		local g = P.gage_for(p - 1) -- gage owned by this player (owner-pointer match)
		if g then
			if train.lock_hp[p] then memory.write_u32(g + G_HP, train.hp[p]); memory.write_u32(g + G_HP_RED, train.hp[p]) end
			if train.lock_ac[p] then memory.write_f32(g + G_AC, train.ac[p]) end
			if train.lock_sp[p] then memory.write_u32(g + G_SP, train.sp[p]) end
			-- engine-native infinite meters: gage+8 flag 0x4 = Ki never spends, 0x2 = AC never spends
			-- (every spend site honors them: CPlayerBase_SpendKi_unlessFlag4@0x1D06F0 / SpendAC_unlessFlag2@0x1D0630).
			-- RMW so the engine's other gage flags survive; re-asserted per frame while checked.
			local fl = memory.read_u32(g + 8)
			local nf = fl
			if train.inf_sp[p] then nf = nf | 0x4 else nf = nf & 0xFFFFFFFB end
			if train.inf_ac[p] then nf = nf | 0x2 else nf = nf & 0xFFFFFFFD end
			if nf ~= fl then memory.write_u32(g + 8, nf) end
		end
		local pp = P.get_player(p - 1)
		local lk = train.lock_pos[p] -- {X,Y,Z} per-axis locks
		if lk[1] or lk[2] or lk[3] then
			if P.player_valid(pp) then
				if lk[1] then memory.write_f32(pp + 48, train.pos[p][1]) end
				if lk[2] then memory.write_f32(pp + 52, train.pos[p][2]) end
				if lk[3] then memory.write_f32(pp + 56, train.pos[p][3]) end
			end
		end
		-- infinite stun: hold any ACTIVE (>0) hit-timer high so the reaction never recovers. Self-gating -- only
		-- touches sub-timers the game already set (>0 = in a stun), so it can't force stun in neutral.
		if train.inf_stun[p] then
			local ht = hit_timer(pp)
			if ht then
				for _, off in ipairs(STUN_OFFS) do
					if memory.read_u32(ht + off) > 0 then memory.write_u32(ht + off, STUN_HOLD) end
				end
			end
		end
		-- (facing lock is handled entirely by the facing_patches() code patch after the loop -- the engine no longer
		-- writes player+64 once Player_FaceOpponentYaw is patched, so no per-frame data hold is needed.)
	end
	facing_patches(train.lock_rot[1] or train.lock_rot[2]) -- stop the face-opponent turn at the source while F is on
end

-- ============================ framestep / sim freeze (engine-native bit 0x4) ============================
-- Whole-battle freeze via the engine's OWN pause flag g_GameFreeze@0x5D4F90 bit 0x4 (the bit the in-game pause
-- menu sets) -- freezes the entire fighter sim while rendering keeps running. Re-asserted every frame so the
-- native menu's unpause can't strand it. bit 0x4 does NOT gate the round timer or the cloth/swing solver (both
-- run unconditionally), so we also NOP those; and the shadow-mgr scene node is skipped while frozen, so we poke
-- its always-on byte to keep floor shadows. Step = clear the bit for exactly one frame. RE'd 1:1 -- see memory
-- sdbz-pause-hud-arch.
local FREEZE_FLAG = 0x5D4F90
local FREEZE_BIT  = 0x4
local TIMER_NOP   = 0x3EA56C               -- round-timer sub.s decrement (separate path from bit 0x4)
local SWING_NOP   = { 0x362A38, 0x362A48 } -- SwingBone PreStep / SolveChain jal (hold cloth pose under freeze)
local SHADOW_BASE = 0x63FE90               -- *(0x63FE90)+36 = CShadowMgr; +56 = "draw even when inactive" byte
local SHADOW_VT   = 0x4EFAC0               -- CShadowMgr vtable guard
local frz = { frozen = false, run_frames = 0, patched = false, step_n = 1 }

function M.frozen() return frz.frozen end

local function set_game_freeze(on) -- data RMW: preserve the engine's other bits (e.g. in-progress hitstop)
	local f = memory.read_u32(FREEZE_FLAG)
	if on then f = f | FREEZE_BIT else f = f & (~FREEZE_BIT & 0xFFFFFFFF) end
	memory.write_u32(FREEZE_FLAG, f)
end

local function freeze_patches(on) -- round-timer + cloth NOPs (recompiler-safe code patches; held for the session)
	if on == frz.patched then return end
	frz.patched = on
	if on then
		engine.patch(TIMER_NOP, 0x00000000)
		engine.patch(SWING_NOP[1], 0x00000000)
		engine.patch(SWING_NOP[2], 0x00000000)
	else
		engine.unpatch(TIMER_NOP); engine.unpatch(SWING_NOP[1]); engine.unpatch(SWING_NOP[2])
	end
end

local function keep_shadows(on)
	local base = memory.read_u32(SHADOW_BASE); if not valid(base) then return end
	local mgr = memory.read_u32(base + 36);    if not valid(mgr) then return end
	if memory.read_u32(mgr) ~= SHADOW_VT then return end -- guard: poke only the validated CShadowMgr
	memory.write_u8(mgr + 56, on and 1 or 0)
end

-- STAGE freeze (the destroyed-Namek floor fix): the stage runs on its OWN updater -- enable flag bit 0x4
-- @stage+4, timescale float @+12 (StageUpdater_SetEnabled_bit4@0x23ACC0 / SetTimeScale@0x23A960) -- and is NOT
-- gated by g_GameFreeze bit 0x4. That's why the elevating floor (and every stage gimmick) kept moving under
-- framestep and could crash a player standing in the rising volume. Chain = StageMgr_GetStageObj 1:1:
-- stage = [[[0x63FEAC]+104]+104]. RMW only bit 0x4, re-asserted per frame while frozen (the engine's KO
-- cinematic also drives this flag; per-frame re-assert wins, and unfreeze restores it).
local STAGE_MGR_PTR = 0x63FEAC
local function stage_obj()
	local a = memory.read_u32(STAGE_MGR_PTR); if not valid(a) then return nil end
	a = memory.read_u32(a + 104);             if not valid(a) then return nil end
	a = memory.read_u32(a + 104);             if not valid(a) then return nil end
	return a
end
local function stage_freeze(frozen)
	local st = stage_obj(); if not st then return end
	local f = memory.read_u32(st + 4)
	local nf = frozen and (f & 0xFFFFFFFB) or (f | 0x4)
	if nf ~= f then memory.write_u32(st + 4, nf) end
end

-- ============================ game speed (engine time-scale) ============================
-- Master sim-rate float (a dt MULTIPLIER) = [[g_pGameContext 0x5022A8]+0x94]+8 (Clock_GetGlobalSimRate@0x2E37D0;
-- memory sdbz-timescale). 1.0 = normal; the engine's own uses: hitstop 0.0, KO slow-mo 0.5, super-flash stage 0.35.
-- The engine writes it only on those EVENT EDGES (GameFreeze latch) and restores 1.0 on expiry, so a per-frame
-- re-write wins -- which also means the slider overrides hitstop/KO slow-mo while active (training tradeoff).
-- The stage runs its OWN scale (stage+12 + dirty bit 0x2 in stage+4) -- drive it too so gimmicks stay in sync.
local speed = { on = false, value = 1.0 }
local CLOCK_CTX = 0x5022A8
local function clock_rate_addr()
	local c = memory.read_u32(CLOCK_CTX); if not valid(c) then return nil end
	local k = memory.read_u32(c + 0x94);  if not valid(k) then return nil end
	return k + 8
end
local function write_speed(v)
	local a = clock_rate_addr(); if a then memory.write_f32(a, v) end
	local st = stage_obj()
	if st then
		memory.write_f32(st + 12, v)
		memory.write_u32(st + 4, memory.read_u32(st + 4) | 0x2) -- timescale dirty bit
	end
end
function M.speed_update()
	if not speed.on or frz.frozen then return end -- the freeze owns the sim while active
	if not P.in_battle() then return end
	write_speed(speed.value)
end

function M.freeze_set(on)
	frz.frozen = on; frz.run_frames = 0
	set_game_freeze(on)
	stage_freeze(on) -- also freeze stage gimmicks (Namek floor etc.)
	keep_shadows(on)
	freeze_patches(on)
end

function M.freeze_step(n)
	if not frz.frozen then M.freeze_set(true) end
	frz.run_frames = frz.run_frames + (n or 1)
end

-- per-frame: hold bit 0x4 (or clear it for one frame to step). Auto-releases if the battle ends (safety).
function M.freeze_update()
	if not frz.frozen then return end
	if not P.in_battle() then
		frz.frozen = false; frz.run_frames = 0
		set_game_freeze(false); stage_freeze(false); keep_shadows(false); freeze_patches(false)
		return
	end
	local advance = frz.run_frames > 0
	set_game_freeze(not advance) -- clear for the step frame so the sim ticks once; else re-hold the freeze
	stage_freeze(not advance)    -- stage gimmicks step in lockstep with the fighters (Namek floor fix)
	keep_shadows(true)
	if advance then frz.run_frames = frz.run_frames - 1 end
end

-- ============================ combo counter lock ============================
-- The live combo counter lives on the DEFENDER (+0x1854 hits, +0x1858 reaction-weight; CHit_AccumDamageCounters
-- @0x1D0860 increments, HUD shows it via HUD_ComboWidget_SetCount when >=2). It resets when the defender escapes
-- the combo-sustain reaction states -- two `sw $zero` in CHit_EnterReactionState @0x1CC078/0x1CC080. NOP both and
-- the counter NEVER resets: it keeps climbing across separate strings (redzep-CT "freeze combo" style). The
-- combo-end side effects (N-HITS popup / score bonus / best-combo record) still fire normally.
local COMBO_RESETS = { 0x1CC078, 0x1CC080 }
local combo_lock = false
local function combo_lock_set(on)
	if on == combo_lock then return end
	combo_lock = on
	if on then
		for _, a in ipairs(COMBO_RESETS) do engine.patch(a, 0x00000000) end
	else
		for _, a in ipairs(COMBO_RESETS) do engine.unpatch(a) end
	end
end

-- ============================ control panes ============================
function M.step_pane()
	if not pane("Framestep", "p_step") then return end
	local frchanged, fr = imgui.Checkbox("Freeze (fighters + bones + projectiles)", frz.frozen)
	if frchanged then M.freeze_set(fr) end
	imgui.SameLine(); if imgui.Button("Step") then M.freeze_step(1) end
	imgui.SameLine(); if imgui.Button("+10") then M.freeze_step(10) end
	-- typed frame-step: enter an exact N and advance that many frames (InputInt has built-in +/- step buttons)
	imgui.SetNextItemWidth(96)
	local nch, nn = imgui.InputInt("##stepn", frz.step_n, 1); if nch then frz.step_n = (nn < 1) and 1 or nn end
	imgui.SameLine(); if imgui.Button("Step N frames") then M.freeze_step(frz.step_n) end
	imgui.TextDisabled("Freezes fighters AND stage gimmicks (Namek floor etc.) while the engine keeps\nrendering, so freecam can orbit a frozen pose. PCSX2 stays unpaused.")
	imgui.Separator()
	-- game-speed slider (engine time-scale; disabled while frozen -- the freeze owns the sim then)
	local spc, spv = imgui.Checkbox("Game speed", speed.on)
	if spc then
		speed.on = spv
		if not spv then write_speed(1.0) end -- restore normal speed the moment it's turned off
	end
	imgui.SameLine(); imgui.SetNextItemWidth(160)
	local svc, svv = imgui.SliderFloat("##gspeed", speed.value, 0.05, 2.0, "%.2fx")
	if svc then speed.value = svv end
	imgui.SetItemTooltip("Engine-native time-scale (the same float behind KO slow-mo / hit-freeze). 1.00 = normal;\nsound pitch follows automatically. While ON it also overrides hitstop and KO slow-mo.\nCtrl+click to type an exact value.")
end

function M.train_pane()
	if not pane("Training (HP / AC / SP)", "p_train") then return end
	-- one stat row: a PER-PLAYER [lock][slider] for P1 then P2 (independent locks). isfloat picks the widget.
	-- Ctrl+click any slider to type an exact value (ImGui temp-input); floats show full precision (no rounding).
	local function stat_row(tag, key, lo, hi, fmt, isfloat, off, off2)
		imgui.Text(tag); imgui.SameLine(40)
		local live = P.in_battle()
		for p = 1, 2 do
			local id = tag .. p -- unique widget ids per row+player (no PushID -> it only takes an int)
			local g = P.gage_for(p - 1) -- gage owned by THIS player (owner-pointer match; nil outside battle)
			local addr = g and (g + off)
			local _, lk = imgui.Checkbox("##lk" .. id, train["lock_" .. key][p]); train["lock_" .. key][p] = lk
			imgui.SetItemTooltip("Lock P" .. p .. " " .. tag .. " (hold this value every frame)")
			-- LIVE-track the real value when not locked, so the slider always reflects the actual HP/AC/SP
			if live and addr and not lk then
				train[key][p] = isfloat and memory.read_f32(addr) or memory.read_u32(addr)
			end
			imgui.SameLine(); imgui.SetNextItemWidth(isfloat and 132 or 96) -- floats wider to fit %.6f
			local ch, v
			if isfloat then ch, v = imgui.SliderFloat("##s" .. id, train[key][p], lo, hi, "P" .. p .. " " .. fmt)
			else ch, v = imgui.SliderInt("##s" .. id, train[key][p], lo, hi, "P" .. p .. " " .. fmt) end
			if ch then
				train[key][p] = v
				if addr then
					if isfloat then memory.write_f32(addr, v) else memory.write_u32(addr, v) end
					if off2 then memory.write_u32(g + off2, v) end -- HP: write the red/chip too so both bars drain
				end
			end
			if p == 1 then imgui.SameLine() end
		end
	end
	stat_row("HP", "hp", 0, 250, "%d", false, G_HP, G_HP_RED)
	stat_row("AC", "ac", 0.0, 80.0, "%.6f", true, G_AC)
	stat_row("SP", "sp", 0, 240, "%d", false, G_SP)
	-- engine-native infinite meters (one gage flag bit each; the engine's own spend functions honor them)
	imgui.Text("Inf"); imgui.SameLine(40)
	for p = 1, 2 do
		local _k; _k, train.inf_sp[p] = imgui.Checkbox("Ki##infk" .. p, train.inf_sp[p])
		imgui.SetItemTooltip("P" .. p .. " infinite Ki (SP) -- engine-native flag: supers/EX moves never spend the gauge")
		imgui.SameLine()
		local _a; _a, train.inf_ac[p] = imgui.Checkbox("AC##infa" .. p, train.inf_ac[p])
		imgui.SetItemTooltip("P" .. p .. " infinite AC -- engine-native flag: actions never spend the action gauge")
		if p == 1 then imgui.SameLine(nil, 24) end
	end
	-- combo counter lock + live readout (the counter lives on the DEFENDER: "on P1" = combo being done TO P1)
	local clc, clv = imgui.Checkbox("Combo counter never resets", combo_lock)
	if clc then combo_lock_set(clv) end
	imgui.SetItemTooltip("Patches out the two combo-reset stores: the counter keeps climbing across separate strings\ninstead of dropping when the dummy recovers. Uncheck to restore normal combo rules.")
	imgui.SameLine()
	local p0, p1 = P.get_player(0), P.get_player(1)
	local c1 = P.player_valid(p0) and memory.read_u32(p0 + 0x1854) or 0
	local c2 = P.player_valid(p1) and memory.read_u32(p1 + 0x1854) or 0
	imgui.TextDisabled(string.format("(on P1: %d | on P2: %d)", c1, c2))
	imgui.TextDisabled("Each player locks independently. Lock = hold the value every frame; drag to set, or Ctrl+click to type\nan exact value. Floats show full precision. In-battle only.")
	imgui.Separator()
	-- stun timers: infinite-stun toggle + LIVE readout of the 4 CPlayerHitTimer sub-timers. The readout is also the
	-- which-is-which map -- hit / block / dizzy the dummy and watch which of t0..t3 jumps (= hitstun / blockstun / dizzy).
	imgui.Text("Stun"); imgui.SameLine(40)
	for p = 1, 2 do
		local _s; _s, train.inf_stun[p] = imgui.Checkbox("Inf##infst" .. p, train.inf_stun[p])
		imgui.SetItemTooltip("P" .. p .. " infinite hitstun / blockstun / dizzy -- holds any active hit-timer so the reaction never recovers")
		imgui.SameLine()
		local ht = hit_timer(P.get_player(p - 1))
		local t = { 0, 0, 0, 0 }
		if ht then for i = 1, 4 do t[i] = memory.read_u32(ht + STUN_OFFS[i]) end end
		imgui.TextDisabled(string.format("P%d [%d %d %d %d]", p, t[1], t[2], t[3], t[4]))
		if p == 1 then imgui.SameLine(nil, 16) end
	end
	imgui.TextDisabled("The 4 numbers are the hit-timers. To map them: hit / block / dizzy the dummy and watch which jumps\n(= hitstun / blockstun / dizzy duration). 'Inf' holds any active timer so the stun never ends.")
	imgui.Separator()
	imgui.Text("Move character (X / Y / Z) -- lock each axis independently:")
	for p = 1, 2 do
		imgui.PushID(p)
		local pp = P.get_player(p - 1)
		local lk = train.lock_pos[p] -- {X,Y,Z}
		local pv = P.player_valid(pp)
		-- live-track each UNLOCKED axis (so the slider shows where the character actually is); held axes keep theirs
		if pv then
			if not lk[1] then train.pos[p][1] = memory.read_f32(pp + 48) end
			if not lk[2] then train.pos[p][2] = memory.read_f32(pp + 52) end
			if not lk[3] then train.pos[p][3] = memory.read_f32(pp + 56) end
		end
		imgui.Text(p == 1 and "P1" or "P2"); imgui.SameLine()
		local _x; _x, lk[1] = imgui.Checkbox("X##lx", lk[1]); imgui.SetItemTooltip("Hold X every frame"); imgui.SameLine()
		local _y; _y, lk[2] = imgui.Checkbox("Y##ly", lk[2]); imgui.SetItemTooltip("Hold Y every frame"); imgui.SameLine()
		local _z; _z, lk[3] = imgui.Checkbox("Z##lz", lk[3]); imgui.SetItemTooltip("Hold Z every frame"); imgui.SameLine()
		local _f; _f, train.lock_rot[p] = imgui.Checkbox("F##lf", train.lock_rot[p]); imgui.SetItemTooltip("Lock Facing -- freeze orientation; stop auto-turning to face the opponent"); imgui.SameLine()
		imgui.SetNextItemWidth(300)
		local chp, x, y, z = imgui.SliderFloat3("##pos", train.pos[p][1], train.pos[p][2], train.pos[p][3], -60.0, 60.0, "%.6f")
		if chp then
			train.pos[p] = { x, y, z }
			if pv then memory.write_f32(pp + 48, x); memory.write_f32(pp + 52, y); memory.write_f32(pp + 56, z) end
		end
		imgui.PopID()
	end
	imgui.TextDisabled("X/Y/Z check = hold that axis every frame; unchecked axes track the live position. Drag to teleport, or\nCtrl+click to type exact. F = lock facing. Writes node local pos (player+48). Pair with freeze+freecam+ortho.")
end

return M
