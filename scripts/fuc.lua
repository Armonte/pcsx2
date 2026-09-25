-- Fate/Unlimited Codes (PS2, SLPM-55108) overlay + training script -- FIRST DRAFT (2026-09-25).
-- Sibling of sdbz.lua; single self-contained file. It uses only the game-agnostic host tables
-- (memory.* project.* draw.* engine.* imgui.* input.*) and the shared lib/ helpers (persist/color/ui).
--
-- HOST NOTE: ScriptHost.cpp currently loads scripts/sdbz.lua unconditionally. This file only runs once the host
-- dispatches per serial (SLPM-55108 -> fuc.lua). As a safety net it also checks a code/data signature of the FUC
-- ELF (is_fuc) and does NOTHING (no reads that write, no patches) on any other game.
--
-- Every address below comes from static RE of the SLPM_551.08 IDB (notes: FUC/notes/FUC_RE.md + sections/*.md) and,
-- where marked [live], from PINE checks in FUC/notes/FUC_LIVE.md. [code] = proven from disassembly only.
-- Anything marked TODO is NOT proven and is either left out or shown as raw data.

local persist = require("persist")
local color   = require("color")
local col     = color.pack

-- ===================================================================================================
-- ADDRESSES (SLPM-55108 only)
-- ===================================================================================================
local A = {
	-- identity (is_fuc): data word at the "STOP" motion-flag name and the `jal Frame_CatchUpIfLagging` in Game_MainLoop
	SIG_STR_ADDR   = 0x501688, SIG_STR_WORD = 0x504F5453, -- "STOP"                                  [code]
	-- Fx10_BuildGeom 0x355820 draws g_RandSeed from the RENDER pass (Fx10_TaskProc -> Fx10_Draw on pass bits 0x1F0):
	-- 4 x `jal Rand_RangeFloat` (+-0.005 cosmetic jitter). Replacing them with `mtc1 $zero,$f0` keeps the sim RNG
	-- identical no matter what is drawn (required for rollback resim / netplay).                          [code]
	FX10_RAND_JALS = { 0x355AA4, 0x355AC4, 0x355DE4, 0x355E04 }, FX10_JAL_WORD = 0x0C0839D0, MTC1_ZERO_F0 = 0x44800000,
	-- In-engine rollback (tools/rollback_cave.py): resim routine in sub_2A1128 (unreferenced RW fn), hooked over
	-- `jal Task_RunMainListNoArg` in Game_MainLoop. Talks to PCSX2's RollbackDevice via syscall.            [code]
	RB_CAVE = 0x2A1128, RB_HOOK = 0x159F98, RB_HOOK_ORIG = 0x0C084498, RB_HOOK_NEW = 0x0C0A844A,
	RB_CAVE_WORDS = { 0x27BDFFE0, 0xFFBF0000, 0xFFB00008, 0xFFB10010, 0x3C035DB2, 0x3463F00D, 0x24040001, 0x0000000C, 0x0040802D, 0x0000882D, 0x12300012, 0x00000000, 0x3C035DB2, 0x3463F00D, 0x24040002, 0x0220282D, 0x0000000C, 0x0C084498, 0x00000000, 0x0C08444C, 0x24040008, 0x3C035DB2, 0x3463F00D, 0x24040003, 0x0220282D, 0x0000000C, 0x26310001, 0x1000FFEE, 0x00000000, 0x3C035DB2, 0x3463F00D, 0x24040004, 0x0000000C, 0x0C084498, 0x00000000, 0xDFBF0000, 0xDFB00008, 0xDFB10010, 0x03E00008, 0x27BD0020 },
	RB_CAVE_ORIG0  = 0x27BDFF20, -- first word of sub_2A1128 as shipped (addiu sp,-0xE0); install refuses otherwise
	RB_INPUT_BLOCK = 0x522E20, RB_INPUT_LEN = 0x360, -- g_PadMerged + g_Pad[] + raw copies (0x522E20..0x523180)
	CATCHUP_JAL    = 0x159F88, CATCHUP_WORD = 0x0C056898, -- jal Frame_CatchUpIfLagging (NOP = 1 sim tick/frame) [code]

	-- players
	SIDE_WORK      = 0x51C858, -- g_SideWork[2]; leader(side) = rd32(rd32(rd32(SIDE_WORK+4*side)+12))   [code][live]
	PLAYER_SLOTS   = 0x51C870, -- 10 x {u32 used, u32 player*}                                          [code][live]
	GAME_MODE      = 0x51D894, -- 6 = practice [live]; 7 survival, 8 mission ...                        [code]

	-- round / frame
	GAME_FLAGS     = 0x51D8A0, -- g_GameWork_Flags: 0x1000 P1 won, 0x2000 P2 won, 0x4000 draw, 0x8000 time over [code]
	ROUND_TIMER    = 0x51D8C4, -- s32 frames left, -1 = infinite                                         [code]
	ROUND_FRAME    = 0x51D8DC, -- battle sim frame counter (stops when paused)                           [code][live]
	ROUND_INDEX    = 0x51D8C0, ROUNDS_TO_WIN = 0x51D8BC,                                                  -- [code]
	RAND_SEED      = 0x522D80, -- g_RandSeed (MSVC LCG *214013+2531011)                                  [code]
	VBLANK_COUNTER = 0x523D90, -- wall-clock vblanks (runs while paused)                                 [code][live]
	HEAP_ARENA     = 0x522D50, -- g_HeapArenaBase (live value 0x005362C0)                                [code][live]
	SYS_HEAP       = 0x522DB0, -- g_SysHeap object                                                        [code]

	-- pause / freeze lever (s16s; written bytewise because the host has no write_u16)
	PAUSE_ACTIVE   = 0x5231C0, PAUSE_REQUEST = 0x5231C2, PAUSE_FORCE = 0x5231C6,                          -- [code][live]

	-- camera / projection
	OVERRIDE_CAM   = 0x51E444, -- g_OverrideCamObj (battle camera object)                                [code]
	SCENE_CAM      = 0x5247A0, -- g_SceneCamObj (fallback)                                                [code]
	CAMOBJ_RWCAM   = 0x334,    -- camObj -> RwCamera*                                                     [code]
	CAMOBJ_RECT    = 0x340,    -- camObj -> sub-raster rect {x,y,w,h} (s32)                               [code]
	CAMOBJ_DBGPRINT= 0x18,     -- camObj+16 (CamCtl) +8 = debug print level = "Debug Display 2"         [code][live]
	RWCAM_PROJ     = 0x14,     -- 1 perspective, 2 parallel                                               [code]
	RWCAM_VIEW     = 0x20,     -- folded view matrix: x'=.20/.30/.40/.50, y'=.24.., z'=.28..             [code]
	RWCAM_FRAME    = 0x04,     -- RwFrame*; frame+0x50 = LTM (right@+0x50, up@+0x60, at@+0x70, pos@+0x80) [code]
	FB_W = 640, FB_H = 448,    -- render target (Video_SetMode640x448)                                   [code]
}

-- player object fields (object size 0x2550)
local P = {
	SLOT = 0x000, SIDE = 0x008, CHAR = 0x00C,                      -- [code][live]
	MOTION = 0x040,            -- motion controller*: flags@+0, time(s)@+8, step@+0xC, act_no@+0x1C       [code][live]
	FLAGS_1DC = 0x1DC,         -- 0x1000 KO'd, 0x200000 facing flipped (L/R swapped), 0x80 inactive       [code]
	FLAGS_1E0 = 0x1E0, FLAGS_1E4 = 0x1E4, FLAGS_1EC = 0x1EC, FLAGS_1F0 = 0x1F0, -- state words; 1EC&1 full invuln [code]
	MAX_HP = 0x234, DMG_FLOOR = 0x238, DAMAGE = 0x23C,             -- damage taken, remaining = max - dmg   [code][live]
	PENDING_DMG = 0x240, MAGIC = 0x24C,                            -- magic f32 0..3                        [code][live]
	POS = 0x460,               -- vec3 f32 (x = fighting axis)                                          [code][live]
	YAW = 0x484,               -- s16 (0x10000 = 360 deg)                                               [code]
	ACTION = 0x53C,            -- current action id (144.. = attacks)                                   [code][live]
	ACTION_PREV = 0x544, DECIDED = 0x560,                          -- +0x560 [live]: 144/145 while attacking, -1 idle
	ACT_FRAMES = 0x5CC,        -- frames in current action                                              [code][live]
	ATKSET = 0xC10,            -- attack set (hitboxes)                                                 [code]
	HURT = 0x10EC, PUSH = 0x12AC, NSHAPE_FIXED = 16, SHAPE_STRIDE = 28, -- 16 spheres each              [code]
	BTN_GAME = 0x14EC, BTN_PRESSED = 0x14D0,                       -- mapped game-button word / pressed      [code]
	HITSTOP = 0x238C, FREEZE2 = 0x2398, SUPERFREEZE = 0x23A8,      -- frames                                 [code]
	COMBO = 0x246C,            -- hits in the combo this player is RECEIVING                             [code]
	INVULN = 0x24B0,           -- invulnerability frames                                                [code]
	ROUNDS_WON = 0x24D0,
	PROJ_LIST = 0x23E8,        -- *(p+0x23E8) = first node {+0 obj (1 = end sentinel), +8 next}          [code]
	PROJ_ATKSET = 0x64,        -- projectile obj +0x64 = its AtkSet (same layout as player+0xC10)        [code]                                                                                  -- [code]
}
-- attack set (player+0xC10) fields
local AS = { FLAGS = 0x000, STARTUP = 0x004, ACTIVE = 0x008, TYPE = 0x308, SHAPES = 0x30C, COUNT = 0x314 } -- [code]
local SHAPE_SIZE = { [1] = 28, [2] = 44, [3] = 36 } -- sphere / capsule / box                           [code]

-- ===================================================================================================
-- settings
-- ===================================================================================================
local CFG_PATH = (SCRIPT_DIR or ".") .. "/fuc.cfg"
local cfg = {
	show_window = true, master = true, controller_nav = false,
	p_boxes = true, p_train = false, p_debug = false, p_state = true, p_rollback = false,
	hurt = true, push = false, attack = true, pos = true, labels = true, hud = true,
	freeze = false, lock_hp = false, lock_magic = false, magic_value = 3.0, lock_timer = false,
	dbg_motion = false, dbg_camera = false, one_tick = false, snap_verify = false, netplay_rng = false, rb_frames = 2,
	box_thickness = 1.5, circle_fill_alpha = 0.25,
	col_hurt   = { 0.24, 0.86, 0.35, 0.80 },
	col_push   = { 0.35, 0.65, 1.00, 0.70 },
	col_attack = { 1.00, 0.24, 0.24, 0.95 },
	col_startup= { 1.00, 0.70, 0.20, 0.60 },
	col_pos    = { 1.00, 1.00, 1.00, 0.90 },
}
local DEFAULTS = {}
for k, v in pairs(cfg) do DEFAULTS[k] = (type(v) == "table") and { v[1], v[2], v[3], v[4] } or v end
local function save_cfg() persist.save(CFG_PATH, cfg) end
persist.load(CFG_PATH, cfg)
cfg.freeze = false -- runtime-only: never come back frozen after a reload
local ui = require("ui").bind(cfg, save_cfg)

-- ===================================================================================================
-- memory helpers
-- ===================================================================================================
local rd32, rdf = memory.read_u32, memory.read_f32
local function s32(v) if v >= 0x80000000 then return v - 0x100000000 end return v end
local function rds(a) return s32(rd32(a)) end
local function ptr_ok(p) return p ~= 0 and memory.valid(p) end
local function wr16(a, v) memory.write_u8(a, v & 0xFF); memory.write_u8(a + 1, (v >> 8) & 0xFF) end

local fuc_cached = nil
local function is_fuc()
	if not memory.ready() then return false end
	if fuc_cached == nil or not fuc_cached then
		fuc_cached = (rd32(A.SIG_STR_ADDR) == A.SIG_STR_WORD)
			and (rd32(A.CATCHUP_JAL) == A.CATCHUP_WORD or rd32(A.CATCHUP_JAL) == 0) -- 0 = our own NOP patch
	end
	return fuc_cached
end

-- side leader (the fighter a side controls); 0 when no battle
local function leader(side)
	local sw = rd32(A.SIDE_WORK + 4 * side); if not ptr_ok(sw) then return 0 end
	local node = rd32(sw + 12); if not ptr_ok(node) then return 0 end
	local p = rd32(node); if not ptr_ok(p) then return 0 end
	return p
end
local function players() return leader(0), leader(1) end

-- ===================================================================================================
-- projection (RenderWare folded view matrix; see camera_render_files.md)
-- ===================================================================================================
local cam = { ok = false }
local function cam_update()
	cam.ok = false
	local obj = rd32(A.OVERRIDE_CAM)
	local rw = ptr_ok(obj) and rd32(obj + A.CAMOBJ_RWCAM) or 0
	if not ptr_ok(rw) then
		obj = rd32(A.SCENE_CAM)
		rw = ptr_ok(obj) and rd32(obj + A.CAMOBJ_RWCAM) or 0
		if not ptr_ok(rw) then return false end
	end
	local m = {}
	for i = 0, 11 do m[i] = rdf(rw + A.RWCAM_VIEW + 4 * i) end -- 0x20..0x5C (w/ pad slots 3,7,11 unused)
	-- rows: [0..2]=right-coef(x',y',z') of p.x, [4..6] of p.y, [8..10] of p.z; translation row at +0x50..
	cam.m = m
	cam.t = { rdf(rw + 0x50), rdf(rw + 0x54), rdf(rw + 0x58) }
	cam.persp = (rd32(rw + A.RWCAM_PROJ) == 1)
	cam.rx, cam.ry = rds(obj + A.CAMOBJ_RECT), rds(obj + A.CAMOBJ_RECT + 4)
	cam.rw, cam.rh = rds(obj + A.CAMOBJ_RECT + 8), rds(obj + A.CAMOBJ_RECT + 12)
	if cam.rw <= 0 or cam.rw > 4096 then cam.rx, cam.ry, cam.rw, cam.rh = 0, 0, A.FB_W, A.FB_H end
	local fr = rd32(rw + A.RWCAM_FRAME)
	if ptr_ok(fr) then cam.right = { rdf(fr + 0x50), rdf(fr + 0x54), rdf(fr + 0x58) } else cam.right = { 1, 0, 0 } end
	cam.obj = obj
	cam.ok = true
	return true
end

-- world -> window pixels (nil when behind the camera)
local function w2s(x, y, z)
	if not cam.ok then return nil end
	local m, t = cam.m, cam.t
	local xp = x * m[0] + y * m[4] + z * m[8] + t[1]
	local yp = x * m[1] + y * m[5] + z * m[9] + t[2]
	local u, v
	if cam.persp then
		local zp = x * m[2] + y * m[6] + z * m[10] + t[3]
		if not (zp > 1e-4) then return nil end
		u, v = xp / zp, yp / zp
	else
		u, v = xp, yp
	end
	if u ~= u or v ~= v or u < -4 or u > 5 or v < -4 or v > 5 then return nil end
	local px = cam.rx + u * cam.rw
	local py = cam.ry + v * cam.rh
	local sx, sy, ok = project.display_to_screen(px / A.FB_W, py / A.FB_H)
	if not ok then return nil end
	return sx, sy
end

-- ===================================================================================================
-- collision shapes (CollShape_*: +0 type, +4 flags(1|2 = active), +8 RwMatrix*)
-- ===================================================================================================
local function xform(mat, ox, oy, oz, pos_only)
	if not ptr_ok(mat) then return ox, oy, oz end -- NULL matrix (capsule ends) = offsets are world coords
	local px, py, pz = rdf(mat + 0x30), rdf(mat + 0x34), rdf(mat + 0x38)
	if pos_only then return px, py, pz end
	return ox * rdf(mat + 0x00) + oy * rdf(mat + 0x10) + oz * rdf(mat + 0x20) + px,
	       ox * rdf(mat + 0x04) + oy * rdf(mat + 0x14) + oz * rdf(mat + 0x24) + py,
	       ox * rdf(mat + 0x08) + oy * rdf(mat + 0x18) + oz * rdf(mat + 0x28) + pz
end

local function screen_radius(cx, cy, cz, r, sx, sy)
	local R = cam.right
	local ex, ey = w2s(cx + R[1] * r, cy + R[2] * r, cz + R[3] * r)
	if not ex then return nil end
	return math.sqrt((ex - sx) ^ 2 + (ey - sy) ^ 2)
end

local function draw_circle_world(cx, cy, cz, r, c, cfill)
	local sx, sy = w2s(cx, cy, cz); if not sx then return end
	local pr = screen_radius(cx, cy, cz, r, sx, sy); if not pr or pr > 4000 then return end
	if cfill then draw.circle2d(sx, sy, pr, cfill, true) end
	draw.circle2d(sx, sy, pr, c, false, cfg.box_thickness)
	return sx, sy
end

-- returns true if drawn
local function draw_shape(sh, c, cfill)
	local typ, fl = rd32(sh), rd32(sh + 4)
	if (fl & 3) ~= 3 then return false end -- CollShape_IsActive*: enabled(1) and active(2)
	if typ == 1 then -- SPHERE: +0xC offset, +0x18 radius
		local x, y, z = xform(rd32(sh + 8), rdf(sh + 0xC), rdf(sh + 0x10), rdf(sh + 0x14), (fl & 4) ~= 0)
		draw_circle_world(x, y, z, rdf(sh + 0x18), c, cfill)
		return true
	elseif typ == 2 then -- CAPSULE: A mat +8 / off +0x10, B mat +0xC / off +0x1C, radius +0x28
		local r = rdf(sh + 0x28)
		local ax, ay, az = xform(rd32(sh + 8), rdf(sh + 0x10), rdf(sh + 0x14), rdf(sh + 0x18), (fl & 4) ~= 0)
		local bx, by, bz = xform(rd32(sh + 0xC), rdf(sh + 0x1C), rdf(sh + 0x20), rdf(sh + 0x24), (fl & 8) ~= 0)
		local s1x, s1y = draw_circle_world(ax, ay, az, r, c, cfill)
		local s2x, s2y = draw_circle_world(bx, by, bz, r, c, cfill)
		if s1x and s2x then
			local pr = screen_radius(ax, ay, az, r, s1x, s1y) or 0
			local dx, dy = s2x - s1x, s2y - s1y
			local len = math.sqrt(dx * dx + dy * dy)
			if len > 0.5 then
				local nx, ny = -dy / len * pr, dx / len * pr
				draw.line2d(s1x + nx, s1y + ny, s2x + nx, s2y + ny, c, cfg.box_thickness)
				draw.line2d(s1x - nx, s1y - ny, s2x - nx, s2y - ny, c, cfg.box_thickness)
			end
		end
		return true
	elseif typ == 3 then -- BOX: +0xC min, +0x18 max, matrix-local unless flag 4 (world AABB)
		local mn = { rdf(sh + 0xC), rdf(sh + 0x10), rdf(sh + 0x14) }
		local mx = { rdf(sh + 0x18), rdf(sh + 0x1C), rdf(sh + 0x20) }
		local mat = ((fl & 4) ~= 0) and 0 or rd32(sh + 8)
		local pts = {}
		for i = 0, 7 do
			local x = (i & 1 ~= 0) and mx[1] or mn[1]
			local y = (i & 2 ~= 0) and mx[2] or mn[2]
			local z = (i & 4 ~= 0) and mx[3] or mn[3]
			local wx, wy, wz = xform(mat, x, y, z, false)
			local sx, sy = w2s(wx, wy, wz); pts[i] = sx and { sx, sy } or false
		end
		local E = { {0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7} }
		for _, e in ipairs(E) do
			local a, b = pts[e[1]], pts[e[2]]
			if a and b then draw.line2d(a[1], a[2], b[1], b[2], c, cfg.box_thickness) end
		end
		return true
	end
	return false
end

local function fill_of(c) return col({ c[1], c[2], c[3], cfg.circle_fill_alpha }) end

local function draw_atkset(as)
	local fl = rd32(as + AS.FLAGS)
	if (fl & 1) == 0 or (fl & 8) ~= 0 then return end
	local cc = (rdf(as + AS.STARTUP) < 0) and cfg.col_attack or cfg.col_startup -- startup timer still running
	local c, f = col(cc), fill_of(cc)
	local typ, arr, n = rd32(as + AS.TYPE), rd32(as + AS.SHAPES), rds(as + AS.COUNT)
	local sz = SHAPE_SIZE[typ]
	if sz and ptr_ok(arr) and n > 0 and n <= 32 then
		for i = 0, n - 1 do draw_shape(arr + i * sz, c, f) end
	end
end

local function draw_player_boxes(p)
	if cfg.hurt then
		local c, f = col(cfg.col_hurt), fill_of(cfg.col_hurt)
		for i = 0, P.NSHAPE_FIXED - 1 do draw_shape(p + P.HURT + i * P.SHAPE_STRIDE, c, f) end
	end
	if cfg.push then
		local c = col(cfg.col_push)
		for i = 0, P.NSHAPE_FIXED - 1 do draw_shape(p + P.PUSH + i * P.SHAPE_STRIDE, c, nil) end
	end
	if cfg.attack then
		draw_atkset(p + P.ATKSET)
		-- projectiles owned by this player (Player_TickProjectiles @0x1DC450)
		local node, guard = rd32(p + P.PROJ_LIST), 0
		while ptr_ok(node) and guard < 32 do
			local obj = rd32(node)
			if obj == 1 or not ptr_ok(obj) then break end
			draw_atkset(obj + P.PROJ_ATKSET)
			node = rd32(node + 8); guard = guard + 1
		end
	end
	if cfg.pos then
		local x, y, z = rdf(p + P.POS), rdf(p + P.POS + 4), rdf(p + P.POS + 8)
		local sx, sy = w2s(x, y, z)
		if sx then
			local c = col(cfg.col_pos)
			draw.line2d(sx - 6, sy, sx + 6, sy, c, 1.5); draw.line2d(sx, sy - 6, sx, sy + 6, c, 1.5)
			if cfg.labels then draw.text(sx + 8, sy - 8, c, (rd32(p + P.SIDE) == 0) and "P1" or "P2") end
		end
	end
end

-- ===================================================================================================
-- state HUD (text; on_frame / GS thread)
-- ===================================================================================================
local C_WHITE, C_YEL, C_BLUE, C_RED = col({1,1,1,1}), col({1,0.9,0.3,1}), col({0.4,0.7,1,1}), col({1,0.35,0.35,1})
local function hud_line(p, tag, x, y)
	local maxhp, dmg = rds(p + P.MAX_HP), rds(p + P.DAMAGE)
	local m = rd32(p + P.MOTION)
	local actno, mframe = -1, -1
	if ptr_ok(m) then actno = rds(m + 0x1C); mframe = math.floor(rdf(m + 8) * 60 + 0.5) end
	local s = string.format("%s HP %d/%d  MAG %.2f  act %d f%d  anim %d f%d  cmd %d",
		tag, maxhp - dmg, maxhp, rdf(p + P.MAGIC), rds(p + P.ACTION), rds(p + P.ACT_FRAMES), actno, mframe, rds(p + P.DECIDED))
	draw.text(x, y, C_WHITE, s)
	local row2 = {}
	local hs, sf, iv, cb = rds(p + P.HITSTOP), rds(p + P.SUPERFREEZE), rds(p + P.INVULN), rds(p + P.COMBO)
	if hs > 0 then row2[#row2 + 1] = "HITSTOP:" .. hs end
	if sf > 0 then row2[#row2 + 1] = "SUPER:" .. sf end
	if iv > 0 then row2[#row2 + 1] = "INVULN:" .. iv end
	if (rd32(p + P.FLAGS_1EC) & 1) ~= 0 then row2[#row2 + 1] = "FULL-INV" end
	if (rd32(p + P.FLAGS_1DC) & 0x200000) ~= 0 then row2[#row2 + 1] = "FLIP" end
	if cb > 0 then row2[#row2 + 1] = "COMBO RECV:" .. cb end
	row2[#row2 + 1] = string.format("btn %08X", rd32(p + P.BTN_GAME))
	row2[#row2 + 1] = string.format("x %.3f y %.3f", rdf(p + P.POS), rdf(p + P.POS + 4))
	local fs = imgui.GetFontSize() + 2
	draw.text(x, y + fs, (iv > 0 or hs > 0) and C_BLUE or C_YEL, table.concat(row2, "  "))
end

local function draw_hud()
	if not cfg.hud then return end
	local p1, p2 = players()
	local x, y = project.display_to_screen(0.02, 0.10)
	local fs = imgui.GetFontSize() + 2
	if p1 ~= 0 then hud_line(p1, "P1", x, y) end
	if p2 ~= 0 then hud_line(p2, "P2", x, y + 2.5 * fs) end
	local t = rds(A.ROUND_TIMER)
	local ts = (t < 0) and "inf" or string.format("%d (%ds)", t, (t + 59) // 60)
	draw.text(x, y + 5 * fs, C_WHITE, string.format("round %d  timer %s  frame %d  rng %08X%s",
		rds(A.ROUND_INDEX), ts, rd32(A.ROUND_FRAME), rd32(A.RAND_SEED), cfg.freeze and "  [FROZEN]" or ""))
end

-- ===================================================================================================
-- training levers (on_frame)
-- ===================================================================================================
local step_pending, step_from = false, 0
local function freeze_write(on)
	if on then wr16(A.PAUSE_REQUEST, 15); wr16(A.PAUSE_FORCE, 0x7FFF)
	else wr16(A.PAUSE_FORCE, 1) end -- C6 == 1 -> next Pause_Update clears request+force [live]
end
local function freeze_set(on) cfg.freeze = on; freeze_write(on); save_cfg() end
local function freeze_step()
	if not cfg.freeze then return end
	step_pending, step_from = true, rd32(A.ROUND_FRAME)
	wr16(A.PAUSE_FORCE, 1) -- let the sim run until ROUND_FRAME advances, then re-freeze below
end
local dbg_motion_was, dbg_cam_was = false, false

local function training_update()
	local p1, p2 = players()
	-- freeze / step (live: one step = exactly one ROUND_FRAME, 10/10)
	if cfg.freeze and p1 == 0 then cfg.freeze = false; freeze_write(false) end -- left the battle: release
	if cfg.freeze then
		if step_pending then
			if rd32(A.ROUND_FRAME) ~= step_from then step_pending = false; freeze_write(true) end
		else
			freeze_write(true)
		end
	end
	for _, p in ipairs({ p1, p2 }) do
		if p ~= 0 then
			if cfg.lock_hp then memory.write_u32(p + P.DAMAGE, 0) end
			if cfg.lock_magic then memory.write_f32(p + P.MAGIC, cfg.magic_value) end
			-- CRF "Debug Display 1" done cleanly: OR DEBUG1|DEBUG2 (0x30) into the motion flags, never STOP [live]
			local m = rd32(p + P.MOTION)
			if ptr_ok(m) then
				local f = rd32(m)
				if cfg.dbg_motion then
					if (f & 0x30) ~= 0x30 then memory.write_u32(m, f | 0x30) end
				elseif dbg_motion_was then
					memory.write_u32(m, f & ~0x30)
				end
			end
		end
	end
	dbg_motion_was = cfg.dbg_motion
	if cfg.lock_timer and rds(A.ROUND_TIMER) >= 0 then memory.write_u32(A.ROUND_TIMER, 99 * 60) end
	-- "Debug Display 2" = battle camera debug print level (camObj+0x18); live: prints Pos/Tar/ANG/dist (CRF's 0x01798AC8 is this field in one heap layout).
	local co = rd32(A.OVERRIDE_CAM)
	if ptr_ok(co) then
		if cfg.dbg_camera then memory.write_u32(co + A.CAMOBJ_DBGPRINT, 1)
		elseif dbg_cam_was then memory.write_u32(co + A.CAMOBJ_DBGPRINT, 0) end
	end
	dbg_cam_was = cfg.dbg_camera
	-- determinism: exactly 1 sim tick per loop (no lag catch-up)
	if cfg.one_tick then engine.patch(A.CATCHUP_JAL, 0) else engine.unpatch(A.CATCHUP_JAL) end
	for _, a in ipairs(A.FX10_RAND_JALS) do
		local w = rd32(a)
		if cfg.netplay_rng and w == A.FX10_JAL_WORD then engine.patch(a, A.MTC1_ZERO_F0)
		elseif not cfg.netplay_rng and w == A.MTC1_ZERO_F0 then engine.unpatch(a) end
	end
end

-- ===================================================================================================
-- control window
-- ===================================================================================================
local function pane(label, key)
	local open = imgui.CollapsingHeader(label, cfg[key] or false)
	if open ~= cfg[key] then cfg[key] = open; save_cfg() end
	return open
end
local function crow(label, show_key, col_key)
	ui.checkbox(label, show_key); imgui.SameLine(168); ui.color_row("##c" .. label, col_key)
end

-- ===================================================================================================
-- in-engine rollback device (engine table `rbdev`, PCSX2 Sdbz/RollbackDevice): Slippi-style game-side resim
-- ===================================================================================================
local RB_LIB_EXCLUDES = require("fuc_rb_lib_excludes")
local rb_installed, rb_mode = false, 0

local function rb_install()
	if rb_installed then return true end
	local hook, first = rd32(A.RB_HOOK), rd32(A.RB_CAVE)
	if hook ~= A.RB_HOOK_ORIG or (first ~= A.RB_CAVE_ORIG0 and first ~= A.RB_CAVE_WORDS[1]) then
		rb_status_msg = string.format("rollback hook NOT installed: hook %08X cave %08X (unexpected code)", hook, first)
		return false
	end
	for i, w in ipairs(A.RB_CAVE_WORDS) do engine.patch(A.RB_CAVE + 4 * (i - 1), w) end
	engine.patch(A.RB_HOOK, A.RB_HOOK_NEW) -- hook last: the routine is complete before anything can call it
	rb_installed = true
	return true
end

local function rb_uninstall()
	if not rb_installed then return end
	engine.unpatch(A.RB_HOOK)
	for i = 1, #A.RB_CAVE_WORDS do engine.unpatch(A.RB_CAVE + 4 * (i - 1)) end
	rb_installed = false
end

-- mode: 0 off, 1 capture only, 2 sync test (roll back cfg.rb_frames every frame and compare)
local function rb_start(mode)
	rbdev.stop()
	if mode == 0 then rb_mode = 0; rb_uninstall(); return end
	rbdev.clear()
	rbdev.add_region(0x3B1080, 0x536280 - 0x3B1080)          -- .data/.bss
	rbdev.add_region(0x5362C0, 0x01FBD000 - 0x5362C0)        -- heap arena (RwHeap + SysHeap), below EE stack
	rbdev.add_exclude(0x00538FA0, 0x200090)                   -- GS/DMA packet buffer (RwHeap)
	rbdev.add_exclude(0x00538660, 0x940)                      -- DMA chain list (RwHeap)
	rbdev.add_exclude(0x01716330, 0x23200)                    -- audio PCM ring A (SysHeap)
	rbdev.add_exclude(0x0175C7B0, 0x23200)                    -- audio PCM ring B (SysHeap)
	rbdev.add_exclude(0x523D90, 8)                            -- vblank / presented-frame counters
	rbdev.add_exclude(0x522E08, 0x14)                         -- vblanks since poll / elapsed / wait target
	for _, r in ipairs(RB_LIB_EXCLUDES) do rbdev.add_exclude(r[1], r[2]) end
	rbdev.set_input_block(A.RB_INPUT_BLOCK, A.RB_INPUT_LEN)
	local p1, p2 = players()
	if p1 ~= 0 then rbdev.add_watch(p1, 0x2550, "P1") end
	if p2 ~= 0 then rbdev.add_watch(p2, 0x2550, "P2") end
	rbdev.add_watch(A.RAND_SEED, 4, "g_RandSeed")
	rbdev.add_watch(0x51D890, 0x110, "g_GameWork")
	rbdev.add_watch(0x51C858, 0x68, "g_SideWork/slots")
	cfg.one_tick = true -- exactly one sim tick per frame while rolling back
	if not rb_install() then return end
	rbdev.start(mode, cfg.rb_frames, true)
	rb_mode = mode
end

-- ===================================================================================================
-- incremental snapshot ring (engine Lua table `snap`) + file command channel for headless testing
-- ===================================================================================================
local function snap_start(wp)
	snap.clear_regions()
	snap.add_region(0x3B1080, 0x536280 - 0x3B1080)          -- .data/.bss
	snap.add_region(0x5362C0, 0x01FBD000 - 0x5362C0)        -- heap arena (RwHeap + SysHeap), below EE stack
	snap.add_exclude(0x00538FA0, 0x200090)                   -- GS/DMA packet buffer (RwHeap)
	snap.add_exclude(0x00538660, 0x940)                      -- DMA chain list (RwHeap)
	snap.add_exclude(0x01716330, 0x23200)                    -- audio PCM ring A (SysHeap)
	snap.add_exclude(0x0175C7B0, 0x23200)                    -- audio PCM ring B (SysHeap)
	snap.add_exclude(0x523D90, 8)                            -- vblank / presented-frame counters
	snap.verify(cfg.snap_verify)
	snap.start(7, wp)                                        -- 7 = Slippi ROLLBACK_MAX_FRAMES
end

-- scripts/snap_cmd.txt: one command per line (start_compare | start_wp | stop | verify on|off | rollback N |
-- status); polled every 15 frames, deleted after running; results go to scripts/snap_status.txt.
local CMD_DIR = (SCRIPT_PATH or ""):match("^(.*)[/\\]") or "."
local cmd_poll = 0
local function snap_commands()
	if not snap then return end
	cmd_poll = cmd_poll + 1
	if cmd_poll < 15 then return end
	cmd_poll = 0
	local path = CMD_DIR .. "/snap_cmd.txt"
	local f = io.open(path, "r")
	if not f then return end
	local text = f:read("*a"); f:close(); os.remove(path)
	for line in text:gmatch("[^\r\n]+") do
		local c, arg = line:match("^%s*(%S+)%s*(%S*)")
		if c == "start_compare" then snap_start(false)
		elseif c == "start_wp" then snap_start(true)
		elseif c == "stop" then snap.stop()
		elseif c == "verify" then cfg.snap_verify = (arg == "on"); snap.verify(cfg.snap_verify)
		elseif c == "rollback" then snap.rollback(tonumber(arg) or 1)
		elseif c == "netrng" then cfg.netplay_rng = (arg == "on")
		elseif c == "rb_synctest" then cfg.rb_frames = tonumber(arg) or cfg.rb_frames; rb_start(2)
		elseif c == "rb_capture" then rb_start(1)
		elseif c == "rb_off" then rb_start(0)
		elseif c == "rb_report" and rbdev then rbdev.report(CMD_DIR .. "/rb_report.txt")
		end
	end
	local o = io.open(CMD_DIR .. "/snap_status.txt", "w")
	if o then o:write(snap.status(), "\n", rbdev and rbdev.status() or "", "\n"); o:close() end
end



local function control_window()
	if not cfg.show_window then return end
	if imgui.Begin("FUC (Lua)", cfg.controller_nav) then
		ui.checkbox("Master", "master"); imgui.SameLine()
		if imgui.Button("Save") then save_cfg() end; imgui.SameLine()
		if imgui.Button("Defaults") then
			for k, v in pairs(DEFAULTS) do cfg[k] = (type(v) == "table") and { v[1], v[2], v[3], v[4] } or v end
			save_cfg()
		end
		if not is_fuc() then imgui.TextDisabled("Not Fate/Unlimited Codes (SLPM-55108) -- idle."); imgui.End(); return end
		local p1, p2 = players()
		imgui.Text(string.format("P1 %08X  P2 %08X  mode %d", p1, p2, rds(A.GAME_MODE)))

		if pane("Boxes", "p_boxes") then
			crow("Hurtboxes (p+0x10EC)", "hurt", "col_hurt")
			crow("Pushboxes (p+0x12AC)", "push", "col_push")
			crow("Attack (active)", "attack", "col_attack")
			imgui.Indent(16); imgui.Text("startup"); imgui.Unindent(16); imgui.SameLine(168); ui.color_row("##cstartup", "col_startup")
			crow("Position (+0x460)", "pos", "col_pos")
			ui.checkbox("Labels", "labels")
			ui.slider_float("Thickness", "box_thickness", 0.5, 4.0, "%.1f")
			ui.slider_float("Fill alpha", "circle_fill_alpha", 0.0, 1.0, "%.2f")
			imgui.TextDisabled(cam.ok and string.format("cam rect %d,%d %dx%d %s", cam.rx, cam.ry, cam.rw, cam.rh,
				cam.persp and "persp" or "ortho") or "no camera")
		end
		if pane("State HUD", "p_state") then ui.checkbox("Show state HUD", "hud") end
		if pane("Training", "p_train") then
			local ch, v = imgui.Checkbox("Freeze (pause lever, no menu)", cfg.freeze); if ch then freeze_set(v) end
			imgui.SameLine(); if imgui.Button("Step") then freeze_step() end
			ui.checkbox("Lock HP full (damage=0)", "lock_hp")
			ui.checkbox("Lock magic", "lock_magic"); imgui.SameLine(); imgui.SetNextItemWidth(120)
			ui.slider_float("##mag", "magic_value", 0.0, 3.0, "%.2f")
			ui.checkbox("Lock round timer (99 s)", "lock_timer")
		end
		if pane("Debug displays", "p_debug") then
			ui.checkbox("Motion debug text (CRF Debug Display 1, clean: flags |= 0x30)", "dbg_motion")
			ui.checkbox("Camera debug text (CRF Debug Display 2, camObj+0x18) [unverified]", "dbg_camera")
		end
		if pane("Rollback / determinism", "p_rollback") then
			ui.checkbox("1 sim tick per frame (NOP lag catch-up @0x159F88)", "one_tick")
			ui.checkbox("netplay-safe render RNG (Fx10 draw no longer consumes g_RandSeed)", "netplay_rng")
			if rbdev then
				imgui.Separator()
				ui.slider_int("rollback frames", "rb_frames", 1, 8)
				if imgui.Button("Sync test") then rb_start(2) end; imgui.SameLine()
				if imgui.Button("Capture only") then rb_start(1) end; imgui.SameLine()
				if imgui.Button("Rollback off") then rb_start(0) end
				imgui.Text(rbdev.status())
				if rb_status_msg then imgui.Text(rb_status_msg) end
			end
			imgui.Text(string.format("round frame %d  vblank %d  rng %08X", rd32(A.ROUND_FRAME), rd32(A.VBLANK_COUNTER), rd32(A.RAND_SEED)))
			imgui.Text(string.format("heap arena %08X  sys heap first blk %08X", rd32(A.HEAP_ARENA), rd32(A.SYS_HEAP + 4)))
			-- Incremental page-snapshot ring (engine: Sdbz/PageSnapshotRing, Lua table `snap`). Regions/excludes =
			-- notes/FUC_WORKING_SET.md (live census): static data+bss + heap arena up to the EE stack, minus the
			-- render/audio buffers and wall-clock counters that must keep running linearly.
			if snap then
				if imgui.Button("Snap: compare") then snap_start(false) end; imgui.SameLine()
				if imgui.Button("Snap: write-protect") then snap_start(true) end; imgui.SameLine()
				if imgui.Button("Stop") then snap.stop() end
				if imgui.Button("Rollback 1") then snap.rollback(1) end; imgui.SameLine()
				if imgui.Button("Rollback 6") then snap.rollback(6) end; imgui.SameLine()
				if ui.checkbox("verify loads", "snap_verify") then snap.verify(cfg.snap_verify) end
				imgui.Text(snap.status())
			else
				imgui.TextDisabled("snap.* not available in this build")
			end
		end
	end
	imgui.End()
end


-- ===================================================================================================
-- host callbacks
-- ===================================================================================================
function on_capture()
	if not cfg.master or not is_fuc() then return end
	if not cam_update() then return end
	local p1, p2 = players()
	if p1 ~= 0 then draw_player_boxes(p1) end
	if p2 ~= 0 then draw_player_boxes(p2) end
end

function on_frame()
	snap_commands()
	engine.set_cursor(cfg.show_window)
	engine.set_gamepad_nav(cfg.controller_nav)
	if not is_fuc() then return end
	training_update()
	if cfg.master then draw_hud() end
end

function on_gui() control_window() end

function on_hotkey(name)
	if not is_fuc() then return end
	if name == "toggle_freeze" then freeze_set(not cfg.freeze)
	elseif name == "frame_step" then freeze_step()
	elseif name == "toggle_window" then cfg.show_window = not cfg.show_window; save_cfg()
	elseif name == "toggle_master" then cfg.master = not cfg.master; save_cfg()
	end
end
