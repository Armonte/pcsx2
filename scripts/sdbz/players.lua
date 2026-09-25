-- SDBZ (SLUS-214.42) player / battle addressing + scene-node helpers.
-- PURE leaf module: depends only on the host globals (memory / project / draw). No cfg, no UI, no other
-- sdbz modules. require'd directly by boxes / hud / camera / train. RE'd; no per-character hardcoding.
local M = {}

M.WS_MATRIX     = 0x508F90 -- g_WorldToScreen: the engine's world->screen matrix, rebuilt every frame
M.PLAYERS       = { 0x5ADFB0, 0x5AFC80 } -- g_Player[0..1]: a FIXED static array (stride 0x1CD0) of two fighter
                                          -- structs. CAUTION: the ARRAY slot is fixed memory, but WHICH slot is
                                          -- "P1" ALTERNATES PER MATCH. The engine assigns each fighter a slot_index
                                          -- (*(player+0x534)+8) at match start (=-1 until then) and the HUD keys
                                          -- P1/P2 by it (bar[5*slot]): slot 0 = left/P1, slot 1 = right/P2. So NEVER
                                          -- address P1/P2 by array index -- use get_player/side_array, which resolve
                                          -- the logical side through slot_index. (grognougnou: "P1/P2 switch after
                                          -- each match" -- this is that.)
M.GAGE          = { 0x5B1A10, 0x5B1A48 } -- g_PlayerGage[0..1] (stride 0x38). +4 = OWNER player ptr; +0x0C white HP,
                                          -- +0x10 AC, +0x1C SP, +0x30 red/chip. Parallel to the player array but
                                          -- ALWAYS bind by the +4 owner pointer (see gage_for) -- never by index.
M.BATTLEMGR_PTR = 0x5022A8 -- *(0x5022A8) = battle mgr; +108 = CPlayerMgr node
M.VT_CPLAYERMGR = 0x4E8160 -- CPlayerMgr vtable (the "real battle running" guard)
M.CAMOBJ        = 0x50075C -- *(0x50075C) = camera object; projection @cam+0x10, params @cam+444..456
M.REMAP         = 0x4a20a0 -- g_ChtBoneRemap (cheat bone index -> skeleton array slot)

local REMAP = M.REMAP

function M.valid(p) return p >= 0x100000 and p < 0x2000000 end
local valid = M.valid

-- live P1/P2 mapping. The engine alternates which array slot is "P1" per match (see M.PLAYERS), so resolve the
-- logical side through slot_index rather than the fixed address. slot_index: 0 = P1 (left HUD bar), 1 = P2.
function M.slot_of(arrayIdx) -- slot_index of array slot 0/1 -> 0/1, or -1 if unassigned/invalid
	local p = M.PLAYERS[arrayIdx + 1]; if not p then return -1 end
	local motion = memory.read_u32(p + 0x534) -- CPlayer.motion (CPlayerMotion*)
	if not valid(motion) then return -1 end
	local s = memory.read_u32(motion + 8)     -- CPlayerMotion.slot_index
	return (s == 0 or s == 1) and s or -1
end
function M.logical_of(arrayIdx) -- array slot -> logical side (0/1); identity fallback when unassigned
	local s = M.slot_of(arrayIdx)
	return (s == 0 or s == 1) and s or arrayIdx
end
function M.side_array(side) -- logical side (0=P1,1=P2) -> the array slot currently holding it
	if M.logical_of(0) == side then return 0 end
	if M.logical_of(1) == side then return 1 end
	return side -- transition / unassigned: identity
end

-- logical side 0/1 (P1/P2) -> fighter base, following the live per-match slot assignment.
function M.get_player(i) return M.PLAYERS[M.side_array(i) + 1] or 0 end

-- gage struct OWNED by player index i (0/1), matched by the gage's +4 owner pointer -- NOT a hardcoded order.
-- The two gage structs are parallel to the player array, but binding by the actual owner back-pointer is swap-proof
-- and self-documenting (an earlier hardcoded "reversed" order had HP/AC/SP editing the wrong player). nil if unbound
-- (e.g. outside battle). Caller adds the field offset (+0x0C HP, +0x10 AC, +0x1C SP, +0x30 chip).
function M.gage_for(i)
	local pl = M.get_player(i) -- array slot currently labelled logical-side i (per-match aware)
	for _, g in ipairs(M.GAGE) do
		if memory.read_u32(g + 4) == pl then return g end
	end
	return nil
end

-- Validated by STRUCTURE (works for every character class, no vtable identity): a class vtable into the
-- ELF image + a live posed skeleton (+1968) + current motion (+1976) + sane world Y (+292).
function M.player_valid(p)
	if not valid(p) then return false end
	local vt = memory.read_u32(p)
	if vt < 0x100000 or vt >= 0x600000 then return false end
	if not valid(memory.read_u32(p + 1968)) or not valid(memory.read_u32(p + 1976)) then return false end
	local py = memory.read_f32(p + 292)
	return py == py and py > -200.0 and py < 600.0 -- py==py rejects NaN
end

-- live-battle gate (so freeze / training never leak into CSS / menus)
function M.in_battle()
	local bm = memory.read_u32(M.BATTLEMGR_PTR)
	if not valid(bm) then return false end
	local mgr = memory.read_u32(bm + 108)
	return valid(mgr) and memory.read_u32(mgr) == M.VT_CPLAYERMGR
end

-- ============================ scene-node bone helpers ============================
function M.bone_node(skel, bone)
	local idx = memory.read_u32(REMAP + 4 * bone)
	return memory.read_u32(skel + 12 + 4 * idx)
end

function M.bone_sphere(skel, bone, cx, cy, cz, r, c)
	if r <= 0.0 or bone >= 23 then return end
	local node = M.bone_node(skel, bone)
	if not valid(node) then return end
	draw.sphere(node + 240, cx, cy, cz, r, c)
end

function M.box_screen(skel, bp)
	local bone = memory.read_u32(bp + 20)
	if bone >= 23 then return nil end
	local node = M.bone_node(skel, bone)
	if not valid(node) then return nil end
	local wx, wy, wz = project.apply_matrix_at(node + 240,
		memory.read_f32(bp), memory.read_f32(bp + 4), memory.read_f32(bp + 8))
	local sx, sy, ok = project.world_to_screen(wx, wy, wz)
	if ok then return sx, sy end
end

return M
