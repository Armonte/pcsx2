-- SDBZ camera control: live H-scale / zoom, orthographic projection, and the fully Lua-driven freecam
-- (engine-camera override). Plus the "Freecam" and "Camera scale" control panes. init(ctx) wires
-- cfg / col / ui / pane; CAMOBJ comes from sdbz.players.
local P = require("sdbz.players")
local CAMOBJ = P.CAMOBJ

local M = {}
local cfg, col, ui, pane, save
local FC_TEXT_COL
local NAV  -- nav-cube palette (built in init)
local CUBE -- nav-cube geometry: 26 chamfered-cube regions (built in init)

-- ===== FreeCAD-style navigation cube (original reimplementation) =====
-- A chamfered cube shown in the corner, oriented to the WORLD axes, that turns with the view. It has 26 clickable
-- regions: 6 labelled FACE octagons + 12 EDGE rectangles + 8 CORNER triangles (the chamfers). Clicking a region snaps
-- the freecam to view the fighters straight down that direction (face -> axis, edge -> 2-axis diagonal, corner ->
-- 3-axis diagonal). Grab + drag the cube to orbit. Built once from a chamfer gap g in [0,1).
-- Region = { v = { {x,y,z}, ... } cube-local verts, n = unit outward normal (= snap direction), kind, label }.
local function build_navcube(g)
	local a = 1.0 - g -- where a chamfered coordinate lands (full coords stay at +/-1)
	local F = {}
	-- SDBZ world: X = right/left, Y = up (top/bottom), Z = front/rear.
	local LBL = { [1] = { [1] = "RIGHT", [-1] = "LEFT" }, [2] = { [1] = "TOP", [-1] = "BOTTOM" }, [3] = { [1] = "FRONT", [-1] = "REAR" } }
	-- 6 face octagons: square face with its 4 corners cut by the chamfer
	local ring = { { a, 1 }, { 1, a }, { 1, -a }, { a, -1 }, { -a, -1 }, { -1, -a }, { -1, a }, { -a, 1 } }
	for ax = 1, 3 do
		local u, v = ax % 3 + 1, (ax % 3 + 1) % 3 + 1
		for _, s in ipairs({ 1, -1 }) do
			local verts = {}
			for _, p in ipairs(ring) do local c = { 0, 0, 0 }; c[ax] = s; c[u] = p[1]; c[v] = p[2]; verts[#verts + 1] = c end
			local n = { 0, 0, 0 }; n[ax] = s
			F[#F + 1] = { v = verts, n = n, kind = "face", label = LBL[ax][s], ax = ax, tu = u, tv = v }
		end
	end
	-- 8 corner triangles (each cube corner, chamfered off -> a small tri; 3 verts, one axis pulled to a per vert)
	local r3 = 1.0 / math.sqrt(3)
	for _, sx in ipairs({ 1, -1 }) do for _, sy in ipairs({ 1, -1 }) do for _, sz in ipairs({ 1, -1 }) do
		local s, tri = { sx, sy, sz }, {}
		for k = 1, 3 do local c = { sx, sy, sz }; c[k] = s[k] * a; tri[#tri + 1] = c end
		F[#F + 1] = { v = tri, n = { sx * r3, sy * r3, sz * r3 }, kind = "corner" }
	end end end
	-- 12 edge rectangles (the chamfer strip along each cube edge)
	local r2 = 1.0 / math.sqrt(2)
	for ax = 1, 3 do
		local p, q = ax % 3 + 1, (ax % 3 + 1) % 3 + 1
		for _, sp in ipairs({ 1, -1 }) do for _, sq in ipairs({ 1, -1 }) do
			local rect = {}
			for _, e in ipairs({ 1, -1 }) do
				local c1 = { 0, 0, 0 }; c1[p] = sp;     c1[q] = sq * a; c1[ax] = e
				local c2 = { 0, 0, 0 }; c2[p] = sp * a; c2[q] = sq;     c2[ax] = e
				if e == 1 then rect[#rect + 1] = c1; rect[#rect + 1] = c2 else rect[#rect + 1] = c2; rect[#rect + 1] = c1 end
			end
			local n = { 0, 0, 0 }; n[p] = sp * r2; n[q] = sq * r2
			F[#F + 1] = { v = rect, n = n, kind = "edge" }
		end end
	end
	return F
end

-- cursor inside a (convex) screen polygon: same sign of edge-cross for every edge (winding-agnostic)
local function point_in_poly(px, py, poly)
	local n, sign = #poly, 0
	for i = 1, n do
		local A, B = poly[i], poly[i % n + 1]
		local cr = (B[1] - A[1]) * (py - A[2]) - (B[2] - A[2]) * (px - A[1])
		if cr ~= 0.0 then
			local s = (cr > 0.0) and 1 or -1
			if sign == 0 then sign = s elseif sign ~= s then return false end
		end
	end
	return true
end

function M.init(ctx)
	cfg, col, ui, pane, save = ctx.cfg, ctx.col, ctx.ui, ctx.pane, ctx.save
	FC_TEXT_COL = col({ 1.0, 1.0, 1.0, 0.88 })
	-- nav-cube palette: light faces, slightly darker chamfers (the bevel reads as shading), accent-blue hover,
	-- dark outlines + labels (matches the FreeCAD nav-cube look).
	NAV = {
		face   = col({ 0.86, 0.88, 0.92, 0.95 }), -- face octagons (lightest)
		chamf  = col({ 0.66, 0.70, 0.77, 0.95 }), -- edge chamfers (the bevel between two faces)
		corner = col({ 0.52, 0.56, 0.63, 0.95 }), -- corner chamfers (darkest -> reads as the cut corner)
		hot    = col({ 0.26, 0.60, 1.00, 0.97 }), -- hovered region
		line   = col({ 0.16, 0.18, 0.23, 1.00 }), -- outlines
		text   = col({ 0.09, 0.10, 0.13, 1.00 }), -- face labels
		ax_x = col({ 0.95, 0.27, 0.27, 1.00 }), ax_xd = col({ 0.55, 0.22, 0.22, 0.65 }), -- X axis (red / dim)
		ax_y = col({ 0.30, 0.85, 0.33, 1.00 }), ax_yd = col({ 0.22, 0.50, 0.24, 0.65 }), -- Y axis (green / dim)
		ax_z = col({ 0.38, 0.56, 1.00, 1.00 }), ax_zd = col({ 0.26, 0.36, 0.60, 0.65 }), -- Z axis (blue / dim)
	}
	CUBE = build_navcube(0.22)
end

local function clampf(v, lo, hi) if v < lo then return lo elseif v > hi then return hi else return v end end

-- ============================ live H-scale / zoom + orthographic ============================
-- *(0x50075C) = camera object; projection matrix @cam+0x10, params @cam+444..456
local camsync = { touched = false, base0 = 0.0, base5 = 0.0, last0 = 0.0, last5 = 0.0 }
local ortho_st = { last = -1 }

function M.cam_scale_update()
	if cfg.cam_ortho then return end -- ortho owns cam+0x10
	if not cfg.cam_hscale_on and not camsync.touched then return end
	local cam = memory.read_u32(CAMOBJ)
	if cam < 0x100000 or cam >= 0x2000000 then return end
	local m0, m5 = memory.read_f32(cam + 0x10), memory.read_f32(cam + 0x10 + 20)
	if camsync.base0 == 0.0 or math.abs(m0 - camsync.last0) > 1e-6 then camsync.base0 = m0 end
	if camsync.base5 == 0.0 or math.abs(m5 - camsync.last5) > 1e-6 then camsync.base5 = m5 end
	if cfg.cam_hscale_on then
		local h = (cfg.cam_hscale > 0.05) and cfg.cam_hscale or 0.05
		local z = cfg.cam_link_zoom and (0.75 / h) or cfg.cam_zoom
		if z < 0.2 then z = 0.2 end
		camsync.last0 = camsync.base0 * (h / 0.75) * z
		camsync.last5 = camsync.base5 * z
		memory.write_f32(cam + 0x10, camsync.last0); memory.write_f32(cam + 0x10 + 20, camsync.last5)
		camsync.touched = true
	else
		memory.write_f32(cam + 0x10, camsync.base0); memory.write_f32(cam + 0x10 + 20, camsync.base5)
		camsync.last0, camsync.last5, camsync.touched = camsync.base0, camsync.base5, false
	end
end

function M.ortho_update()
	local want = cfg.cam_ortho and 1 or 0
	if want == 0 and ortho_st.last <= 0 then ortho_st.last = 0; return end
	local cam = memory.read_u32(CAMOBJ)
	if cam < 0x100000 or cam >= 0x2000000 then return end
	local fov, hsc = memory.read_f32(cam + 456), memory.read_f32(cam + 444)
	local nearp, farp = memory.read_f32(cam + 448), memory.read_f32(cam + 452)
	local fn = farp - nearp
	if not (fn > 1.0) or not (fov > 0.01 and fov < 3.1) or not (hsc > 0.01) then return end
	local f = 1.0 / math.tan(fov * 0.5)
	local Mm = {}
	for i = 0, 15 do Mm[i] = 0.0 end
	if want == 1 then
		local d = (cfg.ortho_dist > 0.5) and cfg.ortho_dist or 0.5
		local b0, b5 = (f * hsc) / d, f / d
		if cfg.cam_hscale_on then -- compose H-scale + zoom on top of ortho (cam_scale_update bows out under ortho)
			local h = (cfg.cam_hscale > 0.05) and cfg.cam_hscale or 0.05
			local z = cfg.cam_link_zoom and (0.75 / h) or cfg.cam_zoom
			if z < 0.2 then z = 0.2 end
			b0 = b0 * (h / 0.75) * z; b5 = b5 * z
		end
		Mm[0] = b0; Mm[5] = b5; Mm[10] = -1.0 / fn; Mm[14] = -nearp / fn; Mm[15] = 1.0
		for i = 0, 15 do memory.write_f32(cam + 0x10 + 4 * i, Mm[i]) end
	elseif ortho_st.last == 1 then -- off-transition: rebuild perspective once
		Mm[0] = f * hsc; Mm[5] = f; Mm[10] = -(farp + nearp) / fn; Mm[11] = -1.0; Mm[14] = -nearp * farp / fn
		for i = 0, 15 do memory.write_f32(cam + 0x10 + 4 * i, Mm[i]) end
	end
	ortho_st.last = want
end

-- ============================ freecam (engine-camera override; fully Lua-driven) ============================
-- cam = *(0x50075C). eye @cam+0x150 (w@+0x15C=1), lookat @cam+0x160. The engine's gluLookAt builds View@cam+0x50
-- -> g_ViewMatrix -> g_WorldToScreen, so moving eye/lookat moves the rendered scene AND our overlay together.
-- To stop the game overwriting eye/lookat we NOP the 6 swc1 in CCameraCtrlGame_ApplyToCamera (FC_STORES) via
-- engine.patch; we also write the VIEW matrix (cam+0x50) and cull frustum (cam+208/560) ourselves so the freecam
-- survives a global freeze (the engine's gluLookAt + frustum build run inside the gated gameplay update). RE'd
-- 1:1 from the native path -- see memory sdbz-freecam-cheatsources.
-- ...0x2B4E68 = the camera ROLL reset (swc1 $f0,0x1A4($s0) in CCameraCtrlGame_ApplyToCamera): NOP it so the game stops
-- zeroing cam+0x1A4 each frame -> we write cam+0x1A4 = roll and the engine's own Camera_BuildViewMatrix@0x1C48C0
-- builds the rolled-up view (Mat44_BuildLookAtView) + orientation quat (cam+0x170). = native camera roll.
local FC_STORES = { 0x2B4E1C, 0x2B4E24, 0x2B4E28, 0x2B4E54, 0x2B4E58, 0x2B4E5C, 0x2B4E68 }
-- FULL CAMERA CONTROL (the camera-steal fix): ALL engine camera controllers (game follow-cam, throw/super/intro/
-- KO-continue demo cams) advance through ONE unconditional vcall in the frame's camera build (sub_2D6FB0, start of
-- CAppMain__m05): lw $t9,0($a0); lw $t9,0x18($t9); jalr $t9 @0x2D7008 (delay slot already nop). NOP that jalr and
-- NOTHING writes the camera anymore -- the freecam owns eye/lookat completely, throws/supers/intros included.
-- (FC_STORES only silences the normal gameplay follow-cam; this is the kill switch for everything else.)
local CAMCTRL_TICK = 0x2D7008
local camlock = { on = false }
function M.camlock_set(on)
	if on == camlock.on then return end
	camlock.on = on
	if on then engine.patch(CAMCTRL_TICK, 0x00000000) else engine.unpatch(CAMCTRL_TICK) end
end
local CAM_ROLL = 0x1A4 -- camera object offset: roll angle (radians); 0 = upright
local fc_pivot -- forward decl (defined below) so freecam_update can follow-aim at the pivot
local VK_SHIFT, VK_CTRL = 0x10, 0x11
local VK_LEFT, VK_UP, VK_RIGHT, VK_DOWN = 0x25, 0x26, 0x27, 0x28
local K_W, K_A, K_S, K_D, K_Q, K_E = 0x57, 0x41, 0x53, 0x44, 0x51, 0x45
local fc = { enabled = false, eye = { 0, 0, 0 }, yaw = 0, pitch = 0, roll = 0, pend_yaw = 0, pend_pitch = 0,
	lock = { false, false, false } } -- per-axis eye freeze (X/Y/Z): locked axis ignores WASD movement
local giz = { down = false } -- view-axis gizmo: previous LMB state for click-edge detection

local function fc_cam()
	local cam = memory.read_u32(CAMOBJ)
	if cam < 0x100000 or cam >= 0x2000000 then return nil end
	return cam
end

function M.freecam_enabled() return fc.enabled end

function M.freecam_set_enabled(on)
	if on == fc.enabled then return end
	if on then
		local cam = fc_cam()
		if not cam then return end -- no camera yet -> ignore
		-- seed freecam from the live game camera so the view doesn't jump
		fc.eye[1] = memory.read_f32(cam + 0x150); fc.eye[2] = memory.read_f32(cam + 0x154); fc.eye[3] = memory.read_f32(cam + 0x158)
		local dx = memory.read_f32(cam + 0x160) - fc.eye[1]
		local dy = memory.read_f32(cam + 0x164) - fc.eye[2]
		local dz = memory.read_f32(cam + 0x168) - fc.eye[3]
		fc.yaw = math.atan(dz, dx)
		fc.pitch = -math.atan(dy, math.sqrt(dx * dx + dz * dz))
		fc.pend_yaw, fc.pend_pitch = 0, 0
		fc.enabled = true
		giz.active = false; giz.moved = false; giz.nub = nil; giz.down = input.mouse_down(0) -- no stale click -> no spurious snap/ortho on (re)enable
		for i = 1, #FC_STORES do engine.patch(FC_STORES[i], 0x00000000) end -- NOP eye/lookat + roll-reset stores (-> nop)
	else
		fc.enabled = false
		for i = 1, #FC_STORES do engine.unpatch(FC_STORES[i]) end
		M.camlock_set(false) -- release the camera-controller kill switch with the freecam (never leave it dangling)
	end
end

function M.freecam_update()
	if not fc.enabled then return end
	local cam = fc_cam()
	if not cam then return end
	local dt60 = 1.0 -- overlay runs at present (~60fps); move speed is world-units/frame and live-tunable

	-- suppress keyboard-driven look/move while typing into an ImGui field (so arrow keys / WASD edit the text field,
	-- not the camera). Mouse-look (RMB) is unaffected. IoFlags = (KeyCtrl, WantTextInput, WantCaptureKeyboard).
	local _, typing = imgui.IoFlags()
	local kb = not typing

	-- scroll wheel -> move speed (log feel); ignore while the pointer is over our window (1:1 native)
	local w = input.wheel()
	if w ~= 0 and not input.want_mouse() then cfg.fc_move_speed = clampf(cfg.fc_move_speed * (1.15 ^ w), 0.05, 64.0) end

	-- look input: RAW RMB cursor delta (recentered each frame -> edge-free infinite look, 1:1 native) + arrow keys,
	-- eased in for smoothing. look_delta() returns (0,0) when RMB is up.
	local dyaw, dpitch = 0.0, 0.0
	local mdx, mdy = input.look_delta()
	dyaw = dyaw + mdx * cfg.fc_mouse_sens
	dpitch = dpitch + mdy * cfg.fc_mouse_sens * (cfg.fc_invert_y and -1.0 or 1.0)
	if kb and input.key_down(VK_LEFT)  then dyaw = dyaw - cfg.fc_look_speed end
	if kb and input.key_down(VK_RIGHT) then dyaw = dyaw + cfg.fc_look_speed end
	if kb and input.key_down(VK_UP)    then dpitch = dpitch - cfg.fc_look_speed end
	if kb and input.key_down(VK_DOWN)  then dpitch = dpitch + cfg.fc_look_speed end
	fc.pend_yaw = fc.pend_yaw + dyaw
	fc.pend_pitch = fc.pend_pitch + dpitch
	local sm = clampf(cfg.fc_look_smooth, 0.0, 0.95)
	local k = (sm <= 0.0) and 1.0 or (1.0 - sm ^ dt60)
	fc.yaw = fc.yaw + fc.pend_yaw * k;       fc.pend_yaw = fc.pend_yaw * (1.0 - k)
	fc.pitch = fc.pitch + fc.pend_pitch * k; fc.pend_pitch = fc.pend_pitch * (1.0 - k)
	if fc.pitch > 1.55 then fc.pitch = 1.55; if fc.pend_pitch > 0 then fc.pend_pitch = 0 end end
	if fc.pitch < -1.55 then fc.pitch = -1.55; if fc.pend_pitch < 0 then fc.pend_pitch = 0 end end

	-- FOLLOW: keep the camera aimed at the snap pivot (chosen target: midpoint / P1 / P2 / stage origin) as it moves.
	-- Overrides look input (mouse/arrows) each frame so the fighter stays centred; WASD still repositions the eye,
	-- so you orbit/dolly while always facing them. gpbear's "snap to character AND stay on them as they move".
	if cfg.fc_follow then
		local px, py, pz = fc_pivot()
		if px then
			local dx, dy, dz = px - fc.eye[1], py - fc.eye[2], pz - fc.eye[3]
			local hlen = math.sqrt(dx * dx + dz * dz)
			if hlen > 1e-4 or math.abs(dy) > 1e-4 then
				fc.yaw = math.atan(dz, dx)
				fc.pitch = clampf(-math.atan(dy, hlen), -1.55, 1.55)
				fc.pend_yaw, fc.pend_pitch = 0, 0 -- discard queued look so it doesn't fight the follow
			end
		end
	end

	local cp, sp = math.cos(fc.pitch), math.sin(fc.pitch)
	local cy, sy = math.cos(fc.yaw), math.sin(fc.yaw)
	local fx, fy, fz = cy * cp, -sp, sy * cp -- forward unit
	local rx, rz = -sy, cy                   -- ground-plane right unit

	-- movement (WASD + Q/E up/down), Shift fast / Ctrl slow; ortho maps W/S to zoom (dolly is a no-op in parallel proj)
	local spd = cfg.fc_move_speed * dt60
	if input.key_down(VK_SHIFT) then spd = spd * 4.0 end
	if input.key_down(VK_CTRL)  then spd = spd * 0.25 end
	local mx, my, mz = 0.0, 0.0, 0.0
	if cfg.cam_ortho then
		local zr = 0.03 * dt60
		if input.key_down(VK_SHIFT) then zr = zr * 3.0 end
		if input.key_down(VK_CTRL)  then zr = zr * 0.25 end
		if kb and input.key_down(K_W) then cfg.ortho_dist = cfg.ortho_dist * (1.0 - zr) end -- zoom in
		if kb and input.key_down(K_S) then cfg.ortho_dist = cfg.ortho_dist * (1.0 + zr) end -- zoom out
		cfg.ortho_dist = clampf(cfg.ortho_dist, 2.0, 4000.0)
	else
		if kb and input.key_down(K_W) then mx = mx + fx; my = my + fy; mz = mz + fz end
		if kb and input.key_down(K_S) then mx = mx - fx; my = my - fy; mz = mz - fz end
	end
	if kb and input.key_down(K_D) then mx = mx + rx; mz = mz + rz end
	if kb and input.key_down(K_A) then mx = mx - rx; mz = mz - rz end
	if kb and input.key_down(K_E) then my = my + 1.0 end
	if kb and input.key_down(K_Q) then my = my - 1.0 end
	-- per-axis lock: a frozen axis ignores WASD movement (typed InputFloat / snaps still set it). Ortho W/S dolly is
	-- a no-op on eye anyway, so this only gates free-move.
	if not fc.lock[1] then fc.eye[1] = fc.eye[1] + mx * spd end
	if not fc.lock[2] then fc.eye[2] = fc.eye[2] + my * spd end
	if not fc.lock[3] then fc.eye[3] = fc.eye[3] + mz * spd end

	-- write eye + lookat (= eye + forward)
	memory.write_f32(cam + 0x150, fc.eye[1]); memory.write_f32(cam + 0x154, fc.eye[2]); memory.write_f32(cam + 0x158, fc.eye[3]); memory.write_f32(cam + 0x15C, 1.0)
	memory.write_f32(cam + 0x160, fc.eye[1] + fx); memory.write_f32(cam + 0x164, fc.eye[2] + fy); memory.write_f32(cam + 0x168, fc.eye[3] + fz); memory.write_f32(cam + 0x16C, 1.0)
	-- roll: the game's roll-reset store is NOP'd (FC_STORES), so cam+0x1A4 persists and Camera_BuildViewMatrix rolls
	-- the view from it. ApplyToCamera still sets the dirty flag each frame, so the rebuild picks this up.
	memory.write_f32(cam + CAM_ROLL, fc.roll)

	-- build + write the VIEW matrix at cam+0x50. Engine convention (RE'd, column-major, right-handed):
	-- F = normalize(eye-lookat) = -forward; S = normalize(cross(up=(0,1,0), F)); U = cross(F,S). columns S/U/F,
	-- 4th row = -dot(basis, eye). Skip near-vertical look (degenerate S) -> keep last view.
	local Fx, Fy, Fz = -fx, -fy, -fz
	local Sx, Sy, Sz = Fz, 0.0, -Fx
	local sl = math.sqrt(Sx * Sx + Sy * Sy + Sz * Sz)
	if sl > 1e-4 then
		Sx, Sy, Sz = Sx / sl, Sy / sl, Sz / sl
		local Ux = Fy * Sz - Fz * Sy
		local Uy = Fz * Sx - Fx * Sz
		local Uz = Fx * Sy - Fy * Sx
		if fc.roll ~= 0.0 then -- roll the right/up basis around the view axis (look dir unchanged), keeping it orthonormal
			local cr, sr = math.cos(fc.roll), math.sin(fc.roll)
			local s1, s2, s3 = Sx * cr + Ux * sr, Sy * cr + Uy * sr, Sz * cr + Uz * sr
			Ux, Uy, Uz = -Sx * sr + Ux * cr, -Sy * sr + Uy * cr, -Sz * sr + Uz * cr
			Sx, Sy, Sz = s1, s2, s3
		end
		local ex, ey, ez = fc.eye[1], fc.eye[2], fc.eye[3]
		local V = {
			Sx, Ux, Fx, 0.0,
			Sy, Uy, Fy, 0.0,
			Sz, Uz, Fz, 0.0,
			-(Sx * ex + Sy * ey + Sz * ez), -(Ux * ex + Uy * ey + Uz * ez), -(Fx * ex + Fy * ey + Fz * ez), 1.0,
		}
		for i = 0, 15 do memory.write_f32(cam + 0x50 + 4 * i, V[i + 1]) end

		-- rebuild cull frustum from the freecam: cam+208 orientation {right,up,fwd}, cam+560 = 5 planes (template
		-- @cam+480 scaled by 1/cull_expand, then rotated by Ot). Only bites while FROZEN (live, the game rebuilds
		-- cam+560 from cam+208 each frame). The hard on/off is the no_cull code patch in patches.update.
		if cfg.fc_fix_cull then
			local Ot = { Sx, Sy, Sz, 0.0,  Ux, Uy, Uz, 0.0,  fx, fy, fz, 0.0,  0.0, 0.0, 0.0, 1.0 }
			for i = 0, 15 do memory.write_f32(cam + 208 + 4 * i, Ot[i + 1]) end
			local ex2 = (cfg.cull_expand > 0.01) and (1.0 / cfg.cull_expand) or 1.0
			for p = 0, 4 do
				local src = cam + 480 + 16 * p
				local dst = cam + 560 + 16 * p
				local t0 = memory.read_f32(src) * ex2
				local t1 = memory.read_f32(src + 4) * ex2
				local t2 = memory.read_f32(src + 8) * ex2
				local t3 = memory.read_f32(src + 12)
				-- row-vector (t0,t1,t2,t3) * Ot  (matches native RowMul)
				local w0 = t0 * Ot[1] + t1 * Ot[5] + t2 * Ot[9]  + t3 * Ot[13]
				local w1 = t0 * Ot[2] + t1 * Ot[6] + t2 * Ot[10] + t3 * Ot[14]
				local w2 = t0 * Ot[3] + t1 * Ot[7] + t2 * Ot[11] + t3 * Ot[15]
				local w3 = t0 * Ot[4] + t1 * Ot[8] + t2 * Ot[12] + t3 * Ot[16]
				memory.write_f32(dst, w0); memory.write_f32(dst + 4, w1); memory.write_f32(dst + 8, w2); memory.write_f32(dst + 12, w3)
			end
			memory.write_f32(cam + 384, fx); memory.write_f32(cam + 388, fy); memory.write_f32(cam + 392, fz) -- forward basis
			memory.write_f32(cam + 400, Sx); memory.write_f32(cam + 404, Sy); memory.write_f32(cam + 408, Sz) -- right basis
		end
	end

	-- on-screen speed indicator (screen-space text, bottom-center)
	local ix, iy = project.display_to_screen(0.30, 0.93)
	draw.text(ix, iy, FC_TEXT_COL, string.format("FREECAM  speed %.2f  (RMB look, scroll=speed, WASD/QE, Shift/Ctrl)", cfg.fc_move_speed))
end

-- ===== Blender-style axis-view snap: orbit the freecam to look straight down a world axis at the fighters =====
-- pivot = midpoint of the two fighters (so the subject stays centred); preserves the current orbit distance.
function fc_pivot()
	-- cfg.snap_pivot: 0 = fighters' midpoint (default), 1 = P1, 2 = P2, 3 = stage origin (0,0,0).
	-- Drives every snap (buttons + nav cube) -> gpbear's "camera snap to character / stage origin".
	local mode = cfg.snap_pivot or 0
	if mode == 3 then return 0.0, 0.0, 0.0 end
	local function wp(p) -- world-matrix translation (player+288/292/296) -> world position
		if not P.player_valid(p) then return nil end
		return memory.read_f32(p + 288), memory.read_f32(p + 292), memory.read_f32(p + 296)
	end
	local x0, y0, z0 = wp(P.get_player(0))
	local x1, y1, z1 = wp(P.get_player(1))
	if mode == 1 and x0 then return x0, y0, z0 end
	if mode == 2 and x1 then return x1, y1, z1 end
	if x0 and x1 then return (x0 + x1) * 0.5, (y0 + y1) * 0.5, (z0 + z1) * 0.5 end
	if x0 then return x0, y0, z0 end
	if x1 then return x1, y1, z1 end
	return nil
end

-- (ax,ay,az) = unit world axis the CAMERA sits on; it looks back toward the pivot (forward F = -axis).
-- keep_proj = don't touch the projection (nav-cube face/edge snaps pass this so they never surprise-flip ortho).
-- iso = force orthographic (a TRUE isometric) -- nav-cube CORNER snaps pass this (corner = iso/axonometric view).
function M.freecam_snap(ax, ay, az, keep_proj, iso)
	if not fc.enabled then M.freecam_set_enabled(true) end
	if not fc.enabled then return end
	local px, py, pz = fc_pivot()
	if not px then -- no fighters -> pivot on the current look-at point so the snap still rotates in place
		px = fc.eye[1] + math.cos(fc.yaw) * math.cos(fc.pitch)
		py = fc.eye[2] - math.sin(fc.pitch)
		pz = fc.eye[3] + math.sin(fc.yaw) * math.cos(fc.pitch)
	end
	local dx, dy, dz = fc.eye[1] - px, fc.eye[2] - py, fc.eye[3] - pz
	local dist = math.sqrt(dx * dx + dy * dy + dz * dz)
	if not (dist > 2.0 and dist < 2000.0) then dist = 40.0 end -- preserve current zoom, sane fallback
	fc.eye[1] = px + ax * dist; fc.eye[2] = py + ay * dist; fc.eye[3] = pz + az * dist
	-- forward F = -axis; invert F = (cos y cos p, -sin p, sin y cos p):  sin p = ay,  yaw = atan2(-az,-ax)
	fc.pitch = clampf(math.asin(clampf(ay, -1.0, 1.0)), -1.55, 1.55)
	fc.yaw = math.atan(-az, -ax)
	fc.roll = 0.0 -- snapping to an axis gives a clean upright view
	fc.pend_yaw, fc.pend_pitch = 0, 0
	if iso or (cfg.snap_ortho and not keep_proj) then cfg.cam_ortho = true; cfg.ortho_dist = clampf(dist, 2.0, 4000.0); save() end
end

-- FreeCAD-style navigation cube (original reimplementation). The chamfered cube (built in init) is oriented to the
-- world axes and projected onto the view's right/up basis, so it turns with the camera. Front-facing regions are
-- filled back-to-front (faces light, chamfers darker), outlined, and the 6 faces are labelled. Hovered region is
-- highlighted; click a region (no drag) to snap the view down it; grab + drag the cube to orbit. Screen-space prims.
-- minimal vector font for the face labels: strokes on a 3-wide x 6-tall grid (y up); only the letters the 6 face
-- words (FRONT REAR TOP BOTTOM LEFT RIGHT) use. Each glyph = list of polylines (flat {x0,y0,x1,y1,...}).
local GLYPH = {
	F = { { 0, 0, 0, 6 }, { 0, 6, 3, 6 }, { 0, 3.2, 2.4, 3.2 } },
	R = { { 0, 0, 0, 6 }, { 0, 6, 2.6, 6, 3, 5.4, 3, 4.2, 2.6, 3.4, 0, 3.4 }, { 1.4, 3.4, 3, 0 } },
	O = { { 0, 1.4, 0, 4.6, 1, 6, 2, 6, 3, 4.6, 3, 1.4, 2, 0, 1, 0, 0, 1.4 } },
	N = { { 0, 0, 0, 6 }, { 0, 6, 3, 0 }, { 3, 0, 3, 6 } },
	T = { { 0, 6, 3, 6 }, { 1.5, 6, 1.5, 0 } },
	E = { { 0, 0, 0, 6 }, { 0, 6, 3, 6 }, { 0, 3.2, 2.4, 3.2 }, { 0, 0, 3, 0 } },
	A = { { 0, 0, 1.5, 6, 3, 0 }, { 0.7, 2.4, 2.3, 2.4 } },
	P = { { 0, 0, 0, 6 }, { 0, 6, 2.6, 6, 3, 5.4, 3, 4.2, 2.6, 3.4, 0, 3.4 } },
	B = { { 0, 0, 0, 6 }, { 0, 6, 2.5, 6, 3, 5.4, 3, 4.2, 2.5, 3.3, 0, 3.3 }, { 0, 3.3, 2.6, 3.3, 3, 2.7, 3, 0.6, 2.5, 0, 0, 0 } },
	M = { { 0, 0, 0, 6 }, { 0, 6, 1.5, 3 }, { 1.5, 3, 3, 6 }, { 3, 6, 3, 0 } },
	L = { { 0, 6, 0, 0 }, { 0, 0, 3, 0 } },
	I = { { 1.5, 0, 1.5, 6 } },
	G = { { 3, 4.6, 2, 6, 1, 6, 0, 4.6, 0, 1.4, 1, 0, 2, 0, 3, 1.4, 3, 3, 1.7, 3 } },
	H = { { 0, 0, 0, 6 }, { 3, 0, 3, 6 }, { 0, 3, 3, 3 } },
}

-- draw `word` centred at (ox,oy) using screen basis vectors H (per grid-unit right) and V (per grid-unit up), so the
-- text rotates with the cube face it sits on. H/V already carry the font scale.
local function render_word(word, ox, oy, Hx, Hy, Vx, Vy, colr, th)
	local n = #word
	local x0 = -(n * 4 - 1) * 0.5 -- advance 4 grid units/letter; centre the whole word
	for i = 1, n do
		local g = GLYPH[word:sub(i, i)]
		if g then
			local cgx = x0 + (i - 1) * 4
			for _, st in ipairs(g) do
				for k = 1, #st - 2, 2 do
					local ax, ay = cgx + st[k], st[k + 1] - 3.0 -- grid -> centred (height 6 -> mid 3)
					local bx, by = cgx + st[k + 2], st[k + 3] - 3.0
					draw.line2d(ox + ax * Hx + ay * Vx, oy + ax * Hy + ay * Vy,
						ox + bx * Hx + by * Vx, oy + bx * Hy + by * Vy, colr, th)
				end
			end
		end
	end
end

-- orbit the freecam by (dyaw,dpitch) about a point on the CURRENT view ray (same as the drag -> consistent + no
-- fling), at the distance of the fighters' midpoint (so the subject stays framed). Used by the rotate arrows.
local function orbit_step(dyaw, dpitch)
	local d, mxp, myp, mzp = 40.0, fc_pivot()
	if mxp then
		local ex, ey, ez = fc.eye[1] - mxp, fc.eye[2] - myp, fc.eye[3] - mzp
		d = math.sqrt(ex * ex + ey * ey + ez * ez)
	end
	if not (d > 2.0 and d < 2000.0) then d = 40.0 end
	local px = fc.eye[1] + math.cos(fc.yaw) * math.cos(fc.pitch) * d
	local py = fc.eye[2] - math.sin(fc.pitch) * d
	local pz = fc.eye[3] + math.sin(fc.yaw) * math.cos(fc.pitch) * d
	fc.yaw = fc.yaw + dyaw
	fc.pitch = clampf(fc.pitch + dpitch, -1.50, 1.50)
	fc.eye[1] = px - math.cos(fc.yaw) * math.cos(fc.pitch) * d
	fc.eye[2] = py + math.sin(fc.pitch) * d
	fc.eye[3] = pz - math.sin(fc.yaw) * math.cos(fc.pitch) * d
	fc.pend_yaw, fc.pend_pitch = 0, 0
end

-- a filled triangle "chevron" arrow pointing along unit (dx,dy)
local function draw_chevron(x, y, dx, dy, s, fill, line)
	local px, py = -dy, dx -- perpendicular
	local tx, ty = x + dx * s, y + dy * s
	local bx, by = x - dx * s * 0.4 + px * s * 0.85, y - dy * s * 0.4 + py * s * 0.85
	local ux, uy = x - dx * s * 0.4 - px * s * 0.85, y - dy * s * 0.4 - py * s * 0.85
	draw.tri2d(tx, ty, bx, by, ux, uy, fill, 0.6)
	draw.line2d(tx, ty, bx, by, line, 1.5); draw.line2d(bx, by, ux, uy, line, 1.5); draw.line2d(ux, uy, tx, ty, line, 1.5)
end

-- a small curved (roll) arrow: ~3/4 arc + an arrowhead at the leading end. `base` rotates the whole glyph.
local function draw_roll(x, y, r, ccw, fill, line, base)
	base = base or 0.0
	local a0, a1 = base - 2.0, base + 2.0
	if ccw then a0, a1 = a1, a0 end
	local prevx, prevy
	for i = 0, 12 do
		local t = a0 + (a1 - a0) * i / 12
		local qx, qy = x + math.cos(t) * r, y + math.sin(t) * r
		if prevx then draw.line2d(prevx, prevy, qx, qy, line, 2.0) end
		prevx, prevy = qx, qy
	end
	local tanx, tany = -math.sin(a1), math.cos(a1); if ccw then tanx, tany = -tanx, -tany end
	draw_chevron(prevx, prevy, tanx, tany, r * 0.6, fill, line)
end

local function unit2(x, y) local l = math.sqrt(x * x + y * y); if l < 1e-4 then l = 1.0 end return x / l, y / l end

function M.gizmo_update()
	giz.hovering = false -- cleared when the gizmo isn't shown so the dbl-click-fullscreen claim releases
	if not fc.enabled or not cfg.show_gizmo then return end
	-- camera basis from yaw/pitch (matches the freecam view build): look dir f, view-fwd Fv=-f, right S, up U
	local cp, sp = math.cos(fc.pitch), math.sin(fc.pitch)
	local cyw, syw = math.cos(fc.yaw), math.sin(fc.yaw)
	local fx, fy, fz = cyw * cp, -sp, syw * cp
	local Fvx, Fvy, Fvz = -fx, -fy, -fz
	local Sx, Sy, Sz = Fvz, 0.0, -Fvx
	local sl = math.sqrt(Sx * Sx + Sy * Sy + Sz * Sz); if sl < 1e-4 then sl = 1.0 end
	Sx, Sy, Sz = Sx / sl, Sy / sl, Sz / sl
	local Ux = Fvy * Sz - Fvz * Sy
	local Uy = Fvz * Sx - Fvx * Sz
	local Uz = Fvx * Sy - Fvy * Sx
	if fc.roll ~= 0.0 then -- match the rolled freecam view so the cube rolls with the scene
		local cr, sr = math.cos(fc.roll), math.sin(fc.roll)
		local s1, s2, s3 = Sx * cr + Ux * sr, Sy * cr + Uy * sr, Sz * cr + Uz * sr
		Ux, Uy, Uz = -Sx * sr + Ux * cr, -Sy * sr + Uy * cr, -Sz * sr + Uz * cr
		Sx, Sy, Sz = s1, s2, s3
	end

	local cx, cyc = project.display_to_screen(0.89, 0.175) -- top-right corner
	local R = 26.0
	-- rotation arrows around the cube: N/S/E/W orbit the view 45deg about the fighters' midpoint; two top-corner roll
	-- arrows spin +/-45deg about the view axis. act() runs on a click (not a drag). Pushed clear of the cube body.
	local AR, STEP, RSTEP = R * 2.25, math.pi / 4, math.pi / 4
	local arrows = {
		{ x = cx, y = cyc - AR, chev = { 0, -1 }, act = function() orbit_step(0, STEP) end },
		{ x = cx, y = cyc + AR, chev = { 0, 1 }, act = function() orbit_step(0, -STEP) end },
		{ x = cx + AR, y = cyc, chev = { 1, 0 }, act = function() orbit_step(STEP, 0) end },
		{ x = cx - AR, y = cyc, chev = { -1, 0 }, act = function() orbit_step(-STEP, 0) end },
		{ x = cx - AR * 0.78, y = cyc - AR * 0.78, roll = true, rrot = 0.0, act = function() fc.roll = fc.roll - RSTEP end },
		{ x = cx + AR * 0.78, y = cyc - AR * 0.78, roll = false, rrot = math.pi, act = function() fc.roll = fc.roll + RSTEP end },
	}

	-- sensitivity so the cube SURFACE tracks the cursor ~1:1 (trackball feel): a point at radius ~R*1.4 px moves
	-- R*1.4*dtheta per radian, so dtheta = drag_px / (R*1.4). Larger divisor = slower. (was a flat 0.01 = way too slow.)
	local DRAG_SENS = 1.0 / (R * 1.4)
	local mx, my = input.mouse_pos()
	local lmb = input.mouse_down(0)
	local press = lmb and not giz.down
	local release = (not lmb) and giz.down
	giz.down = lmb
	-- the gizmo OWNS the mouse within its own bounds (it claims it below so the game won't react). Gate on THAT, not on
	-- want_mouse() -- want_mouse is true whenever ImGui's WantCaptureMouse is set, which was killing the gizmo's own clicks.
	local zdx, zdy = mx - cx, my - cyc
	local in_zone = (zdx * zdx + zdy * zdy) <= (AR + 14.0) * (AR + 14.0)

	-- project + back-face cull every region; collect the front-facing ones with their screen polygon + view depth
	local function proj(P)
		return cx + (P[1] * Sx + P[2] * Sy + P[3] * Sz) * R,
		       cyc - (P[1] * Ux + P[2] * Uy + P[3] * Uz) * R
	end
	local vis = {}
	for _, rg in ipairs(CUBE) do
		local d = rg.n[1] * Fvx + rg.n[2] * Fvy + rg.n[3] * Fvz -- >0 = normal toward viewer (front-facing)
		if d > 0.0 then
			local poly, sxs, sys = {}, 0.0, 0.0
			for i, P in ipairs(rg.v) do local x, y = proj(P); poly[i] = { x, y }; sxs = sxs + x; sys = sys + y end
			vis[#vis + 1] = { rg = rg, poly = poly, depth = d, lx = sxs / #poly, ly = sys / #poly }
		end
	end
	table.sort(vis, function(a, b) return a.depth < b.depth end) -- back-to-front

	-- hover: the front-most cube region under the cursor takes priority (so grabbing/clicking the cube always wins);
	-- a rotation arrow is hot only when the cursor is NOT over the cube body.
	local hot, arrow_hot
	if in_zone then
		-- ARROWS FIRST: a click dead-on an arrow must rotate, never get swallowed as a cube grab (that was the bug --
		-- the arrow click was grabbing the cube, so orbit_step never ran). Arrows sit outside the cube, so this
		-- doesn't steal cube/drag clicks. Cube is checked only when no arrow is under the cursor.
		for _, ar in ipairs(arrows) do
			local ddx, ddy = mx - ar.x, my - ar.y
			if ddx * ddx + ddy * ddy <= 169.0 then arrow_hot = ar; break end -- ~13px hit radius
		end
		if not arrow_hot then
			-- corners are tiny chamfer triangles -> point_in_poly almost never catches them. Give every front-facing
			-- corner a generous circular hit zone at its screen position; nearest one to the cursor wins.
			local bestd2 = 11.0 * 11.0
			for _, it in ipairs(vis) do
				if it.rg.kind == "corner" then
					local dx, dy = mx - it.lx, my - it.ly; local d2 = dx * dx + dy * dy
					if d2 <= bestd2 then hot = it; bestd2 = d2 end
				end
			end
			-- otherwise the front-most face/edge polygon under the cursor
			if not hot then
				for i = #vis, 1, -1 do if point_in_poly(mx, my, vis[i].poly) then hot = vis[i]; break end end
			end
		end
	end

	-- click a rotation arrow to rotate; else grab the cube to ORBIT (a click on a region without dragging = snap)
	local gr = R * 1.65
	local cdx, cdy = mx - cx, my - cyc
	local in_giz = (cdx * cdx + cdy * cdy) <= gr * gr
	if press and in_zone and arrow_hot then
		arrow_hot.act()
	elseif press and in_zone and (hot or in_giz) then -- grab the cube: cache the orbit pivot + distance at press
		giz.active = true; giz.moved = false; giz.nub = hot and hot.rg or nil
		giz.mx0, giz.my0, giz.yaw0, giz.pitch0 = mx, my, fc.yaw, fc.pitch -- absolute drag anchor (sticky 1:1 feel)
		-- orbit DISTANCE = how far the fighters' midpoint is (keeps the subject framed); fall back to 40.
		local d, mxp, myp, mzp = 40.0, fc_pivot()
		if mxp then
			local ex, ey, ez = fc.eye[1] - mxp, fc.eye[2] - myp, fc.eye[3] - mzp
			d = math.sqrt(ex * ex + ey * ey + ez * ez)
		end
		if not (d > 2.0 and d < 2000.0) then d = 40.0 end
		-- anchor the pivot ON the current view ray at that distance: the eye is then already exactly pivot-forward*d,
		-- so orbiting starts with ZERO jump (the old reframe-to-midpoint snap was the "awkward" part).
		giz.px = fc.eye[1] + math.cos(fc.yaw) * math.cos(fc.pitch) * d
		giz.py = fc.eye[2] - math.sin(fc.pitch) * d
		giz.pz = fc.eye[3] + math.sin(fc.yaw) * math.cos(fc.pitch) * d
		giz.dist = d
	end
	if giz.active and lmb then
		local odx, ody = mx - giz.mx0, my - giz.my0 -- TOTAL offset from the grab point (absolute -> sticky, no delta drift)
		if odx * odx + ody * ody > 9.0 then giz.moved = true end -- >3px -> a drag, not a click
		if giz.moved then -- yaw/pitch driven directly by how far the cursor is from where you grabbed (1:1 track)
			fc.yaw = giz.yaw0 + odx * DRAG_SENS
			fc.pitch = clampf(giz.pitch0 + ody * DRAG_SENS, -1.50, 1.50)
			fc.eye[1] = giz.px - math.cos(fc.yaw) * math.cos(fc.pitch) * giz.dist
			fc.eye[2] = giz.py + math.sin(fc.pitch) * giz.dist
			fc.eye[3] = giz.pz - math.sin(fc.yaw) * math.cos(fc.pitch) * giz.dist
			fc.pend_yaw, fc.pend_pitch = 0, 0
		end
	end
	if release then
		if giz.active and not giz.moved and giz.nub then M.freecam_snap(giz.nub.n[1], giz.nub.n[2], giz.nub.n[3], true) end
		giz.active = false
	end
	giz.hovering = in_zone or giz.active -- cursor within the gizmo bounds -> claim it (no dbl-click fullscreen)

	-- draw fills back-to-front (faces light, chamfers darker, hover accent), then outlines, then the face labels.
	for _, it in ipairs(vis) do
		local hov = (it == hot) or (giz.active and giz.nub == it.rg)
		local fill = hov and NAV.hot or (it.rg.kind == "face" and NAV.face or (it.rg.kind == "edge" and NAV.chamf or NAV.corner))
		local td, p = 2.0 - it.depth, it.poly -- td = per-face painter depth (farther regions drawn first)
		for i = 2, #p - 1 do draw.tri2d(p[1][1], p[1][2], p[i][1], p[i][2], p[i + 1][1], p[i + 1][2], fill, td) end
		for i = 1, #p do local A, B = p[i], p[i % #p + 1]; draw.line2d(A[1], A[2], B[1], B[2], NAV.line, 1.5) end
	end
	-- coordinate axes (FreeCAD-style): coloured X/Y/Z lines from the cube centre with a ball + letter at each tip;
	-- back-pointing axes are dimmed. (Lines/text blit over the fills, so it reads as an axis cross on the cube.)
	local AX = { { 1, 0, 0, "X", NAV.ax_x, NAV.ax_xd }, { 0, 1, 0, "Y", NAV.ax_y, NAV.ax_yd }, { 0, 0, 1, "Z", NAV.ax_z, NAV.ax_zd } }
	for _, a in ipairs(AX) do
		local dpt = a[1] * Fvx + a[2] * Fvy + a[3] * Fvz
		local cc = (dpt >= -0.05) and a[5] or a[6]
		local tx, ty = proj({ a[1] * 1.7, a[2] * 1.7, a[3] * 1.7 })
		draw.line2d(cx, cyc, tx, ty, cc, 2.0)
		draw.circle2d(tx, ty, 3.5, cc, true)
		draw.text(tx - 3.0, ty - 7.0, cc, a[4])
	end

	-- face labels (vector letters that lie on the face): pick the in-plane axes' screen dirs as the text basis, oriented
	-- so it reads upright + non-mirrored, scaled to fit the face.
	local Sv, Uv = { Sx, Sy, Sz }, { Ux, Uy, Uz }
	for _, it in ipairs(vis) do
		if it.rg.kind == "face" then
			local u, v = it.rg.tu, it.rg.tv
			local gux, guy = unit2(Sv[u], -Uv[u]) -- screen dir of +u world axis
			local gvx, gvy = unit2(Sv[v], -Uv[v])
			local cset = { { gux, guy, "u" }, { -gux, -guy, "u" }, { gvx, gvy, "v" }, { -gvx, -gvy, "v" } }
			local up = cset[1]; for _, c in ipairs(cset) do if c[2] < up[2] then up = c end end -- most screen-up
			local rt = nil
			for _, c in ipairs(cset) do
				if c[3] ~= up[3] and (c[1] * up[2] - c[2] * up[1]) < 0.0 then rt = c; break end -- non-mirrored basis
			end
			if not rt then for _, c in ipairs(cset) do if c[3] ~= up[3] then rt = c; break end end end
			local n = #it.rg.label
			local sc = math.min(R * 0.58 / 6.0, R * 1.5 / (n * 4 - 1)) -- fit ~face height / width
			render_word(it.rg.label, it.lx, it.ly, rt[1] * sc, rt[2] * sc, up[1] * sc, up[2] * sc, NAV.text, 1.4)
		end
	end

	-- rotation arrows (on top): N/S/E/W chevrons + two corner roll arrows; hovered = accent
	for _, ar in ipairs(arrows) do
		local fill = (ar == arrow_hot) and NAV.hot or NAV.chamf
		if ar.chev then draw_chevron(ar.x, ar.y, ar.chev[1], ar.chev[2], 8.0, fill, NAV.line)
		else draw_roll(ar.x, ar.y, 8.0, ar.roll, fill, NAV.line, ar.rrot) end
	end
end

-- true while the cursor is over (or dragging) the gizmo -> sdbz.lua tells the host to skip dbl-click fullscreen.
function M.gizmo_hovering() return giz.hovering end

-- ============================ control panes ============================
function M.freecam_pane()
	if not pane("Freecam", "p_fcam") then return end
	local fcc, fcv = imgui.Checkbox("Enable freecam", fc.enabled)
	if fcc then M.freecam_set_enabled(fcv) end
	if fc.enabled then
		local clc, clv = imgui.Checkbox("Full camera control (stop ALL engine camera writers)", camlock.on)
		if clc then M.camlock_set(clv) end
		imgui.SetItemTooltip("Kills the single call every camera controller runs through -- the camera no longer moves during\nthrows, supers, intros, stage transitions or the KO/continue countdown. Freecam owns it completely.\nReleases automatically when freecam is turned off.")
	end
	imgui.TextDisabled("Move WASD + Q/E (up/down), look arrows or RMB.\nShift=fast, Ctrl=slow. Free-run only (not paused).")
	ui.slider_float("Move speed", "fc_move_speed", 0.25, 16.0, "%.2f")
	ui.slider_float("Mouse sensitivity", "fc_mouse_sens", 0.0005, 0.015, "%.4f")
	ui.slider_float("Look smoothing", "fc_look_smooth", 0.0, 0.95, "%.2f")
	ui.slider_float("Look speed (arrows)", "fc_look_speed", 0.005, 0.12, "%.3f")
	ui.checkbox("Invert Y", "fc_invert_y")
	if imgui.Button("Re-seed from game cam") then if fc.enabled then M.freecam_set_enabled(false); M.freecam_set_enabled(true) end end
	if fc.enabled then
		-- type an EXACT eye position (world units) / orientation (degrees). Applied next frame; WASD/RMB still adjust
		-- on top. Yaw/pitch are derived live, so they update as you mouse-look.
		imgui.SetNextItemWidth(92); local exc, exv = imgui.InputFloat("X##fce", fc.eye[1], 0.0, "%.4f"); if exc then fc.eye[1] = exv end
		imgui.SameLine(); imgui.SetNextItemWidth(92); local eyc, eyv = imgui.InputFloat("Y##fce", fc.eye[2], 0.0, "%.4f"); if eyc then fc.eye[2] = eyv end
		imgui.SameLine(); imgui.SetNextItemWidth(92); local ezc, ezv = imgui.InputFloat("Z##fce", fc.eye[3], 0.0, "%.4f"); if ezc then fc.eye[3] = ezv end
		-- per-axis eye lock: freeze the camera's X / Y / Z so WASD only slides it along the free axes
		imgui.Text("Lock axis:"); imgui.SameLine()
		local _lx; _lx, fc.lock[1] = imgui.Checkbox("X##fclk", fc.lock[1]); imgui.SetItemTooltip("Freeze the camera's X position (WASD won't move it along X)"); imgui.SameLine()
		local _ly; _ly, fc.lock[2] = imgui.Checkbox("Y##fclk", fc.lock[2]); imgui.SetItemTooltip("Freeze the camera's Y (height) -- e.g. keep a level pan"); imgui.SameLine()
		local _lz; _lz, fc.lock[3] = imgui.Checkbox("Z##fclk", fc.lock[3]); imgui.SetItemTooltip("Freeze the camera's Z position")
		local yawd, pitd = math.deg(fc.yaw), math.deg(fc.pitch)
		imgui.SetNextItemWidth(92); local ywc, ywv = imgui.InputFloat("Yaw##fc", yawd, 0.0, "%.2f"); if ywc then fc.yaw = math.rad(ywv) end
		imgui.SameLine(); imgui.SetNextItemWidth(92); local ptc, ptv = imgui.InputFloat("Pitch##fc", pitd, 0.0, "%.2f"); if ptc then fc.pitch = clampf(math.rad(ptv), -1.55, 1.55) end
		imgui.TextDisabled("Type exact eye XYZ (world) or Yaw/Pitch (deg). Ctrl/Shift modify move speed; arrows/WASD off while typing.")
		imgui.Separator()
		imgui.Text("Snap view:"); imgui.SameLine()
		if imgui.Button("+X") then M.freecam_snap(1, 0, 0) end; imgui.SameLine()
		if imgui.Button("-X") then M.freecam_snap(-1, 0, 0) end; imgui.SameLine()
		if imgui.Button("+Y") then M.freecam_snap(0, 1, 0) end; imgui.SameLine()
		if imgui.Button("-Y") then M.freecam_snap(0, -1, 0) end; imgui.SameLine()
		if imgui.Button("+Z") then M.freecam_snap(0, 0, 1) end; imgui.SameLine()
		if imgui.Button("-Z") then M.freecam_snap(0, 0, -1) end
		-- snap/orbit pivot: what the snap buttons + nav cube rotate around (checkboxes act as radio buttons)
		imgui.Text("Snap pivot:"); imgui.SameLine()
		local pivots = { "Midpoint", "P1", "P2", "Stage origin" }
		for i = 1, 4 do
			local v = i - 1
			local pch, pon = imgui.Checkbox(pivots[i] .. "##piv", (cfg.snap_pivot or 0) == v)
			if pch and pon then cfg.snap_pivot = v; save() end
			if i < 4 then imgui.SameLine() end
		end
		imgui.SetItemTooltip("What snaps and the nav cube orbit around: the fighters' midpoint, one fighter, or the\nstage origin (0,0,0). Snap again after a fighter moves to re-center on them.")
		ui.checkbox("Follow pivot (stay aimed as it moves)", "fc_follow")
		imgui.SetItemTooltip("Continuously re-aims the camera at the snap pivot every frame, so the chosen target stays centred\nas it moves. WASD still repositions the eye (orbit/dolly); mouse/arrow look is overridden while on.")
		ui.checkbox("Ortho on snap", "snap_ortho"); imgui.SameLine(); ui.checkbox("Navigation cube", "show_gizmo")
		imgui.SetItemTooltip("FreeCAD-style nav cube (top-right) that turns with the view. Click a face/edge/corner to\nsnap to that direction; grab + drag the cube to orbit.")
		imgui.TextDisabled("Snap orbits the freecam to look down a world axis at the fighters' midpoint. +X/-X/+Z/-Z =\nside/front, +Y/-Y = top/bottom. 'Ortho on snap' = clean 2D-style view. Gizmo = same, clickable.")
	end
	imgui.Separator()
	ui.checkbox("Disable culling (no edge pop-in)", "no_cull")
	imgui.SetItemTooltip("Patches the per-object cull (Cull_SphereVsFrustum) to pass everything. Works ANY time -- live or\npaused, freecam or not. This is the hard kill switch. Ortho enables it automatically.")
	imgui.Separator()
	ui.checkbox("Fix cull (rebuild frustum from freecam)", "fc_fix_cull")
	imgui.SetItemTooltip("Rebuilds the cull frustum (cam+560) from the freecam so it stops culling from the paused viewpoint.")
	imgui.BeginDisabled(not cfg.fc_fix_cull or cfg.no_cull)
	ui.slider_float("Cull expand", "cull_expand", 1.0, 20.0, "%.1fx")
	imgui.EndDisabled()
	imgui.TextDisabled("Expand = graduated 'cull a bit less' (1 = exact game). NOTE: only bites while FROZEN -- live, the\ngame rebuilds the frustum each frame. For a hard off any time, use 'Disable culling' above.")
end

function M.cam_pane()
	if not pane("Camera scale (widescreen / FOV)", "p_cam") then return end
	ui.checkbox("Override camera (H-scale + zoom)", "cam_hscale_on")
	ui.slider_float("H-scale", "cam_hscale", 0.45, 1.10, "%.3f")
	ui.checkbox("Link zoom to H-scale", "cam_link_zoom")
	if cfg.cam_link_zoom then
		local linked = 0.75 / ((cfg.cam_hscale > 0.05) and cfg.cam_hscale or 0.05)
		imgui.BeginDisabled(true); imgui.SliderFloat("Zoom", linked, 0.40, 1.60, "%.2f (linked)"); imgui.EndDisabled()
	else
		ui.slider_float("Zoom", "cam_zoom", 0.40, 1.60, "%.2f")
	end
	imgui.TextDisabled("H-scale = 1/display-aspect: 0.75=4:3, 0.70=10:7, 0.875=8:7, 0.469=16:9.")
	imgui.Separator()
	ui.checkbox("Orthographic (parallel) projection", "cam_ortho")
	imgui.SetItemTooltip("Parallel projection -- no perspective, no depth foreshortening.\nGreat with freecam for clean stage/asset captures. Restores on toggle off.")
	imgui.BeginDisabled(not cfg.cam_ortho)
	ui.slider_float("Ortho distance", "ortho_dist", 2.0, 4000.0, "%.1f", true)
	imgui.SameLine()
	if imgui.Button("Match view") then
		local cam = memory.read_u32(CAMOBJ)
		if cam >= 0x100000 and cam < 0x2000000 then
			local dx = memory.read_f32(cam + 0x150) - memory.read_f32(cam + 0x160)
			local dy = memory.read_f32(cam + 0x154) - memory.read_f32(cam + 0x164)
			local dz = memory.read_f32(cam + 0x158) - memory.read_f32(cam + 0x168)
			local d = math.sqrt(dx * dx + dy * dy + dz * dz)
			if d > 5.0 and d < 1000.0 then cfg.ortho_dist = d; save() end
		end
	end
	imgui.EndDisabled()
	imgui.TextDisabled("Smaller distance = larger subject. 'Match view' sets it to the current eye->lookat distance.")
end

return M
