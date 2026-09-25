-- SDBZ (SLUS-214.42) hitbox overlay -- FULLY SELF-CONTAINED Lua mod, split into modules.
-- This entry owns the settings (cfg + persistence), the ImGui control window, and the four host callbacks
-- (on_capture / on_frame / on_gui / on_hotkey). Everything game-specific lives in sdbz/<module>.lua:
--   sdbz.players  -- player/battle addressing + scene-node helpers (pure leaf)
--   sdbz.boxes    -- hurt/attack/throw/skeleton/projectile drawing + the box-colors pane
--   sdbz.hud      -- state-flag HUD (invuln / guard)
--   sdbz.camera   -- H-scale / zoom / ortho projection + freecam + their panes
--   sdbz.train    -- HP/AC/SP locks, position + facing hold, sim-freeze/framestep + their panes
--   sdbz.patches  -- hide-HUD / no-cull / deinterlace code-patch toggles
-- Nothing here is in native C++. A second game (Fate/Unlimited Codes, ...) is a sibling entry script.
--
-- Host tables: memory.* project.* draw.* engine.* imgui.* input.*  + globals SCRIPT_DIR / SCRIPT_PATH.
-- require() resolves <script_dir>/?.lua then <script_dir>/lib/?.lua, so "sdbz.boxes" -> sdbz/boxes.lua and
-- "persist" -> lib/persist.lua.

local persist = require("persist") -- table <-> .cfg file
local color   = require("color")   -- {r,g,b,a} -> ImU32
local col     = color.pack

-- ============================ settings (owned + persisted here) ============================
local CFG_PATH = (SCRIPT_DIR or ".") .. "/sdbz.cfg"
local cfg = {
	show_window = true,
	master = true, -- master box-draw gate (panel still shows); separate from the script enable/window hotkeys
	controller_nav = false, -- let a gamepad navigate THIS window (off by default so the pad keeps driving the game)
	-- collapsible-pane open states (persisted, 1:1 with the native control window)
	p_boxes = true, p_step = true, p_train = false, p_fcam = false, p_cam = false,
	p_aspect = false, p_deint = false, p_style = false, p_frame = false, p_stage = false, p_rollback = false,
	hurt = true, attack = true, throw = true, prox = true, other = true, labels = true,
	skel = true, shells = true, state = false, -- state HUD (P1/P2 INVULN/guard text) off by default; toggle in Box types
	stage = false, stage_floor = true, stage_walls = true, stage_obst = true, stage_hidden = "", -- stage collision overlay (+ per-node hide-set)
	stage_brk_grid = 0x5BE138, -- the gimmick (Hitgmk*) collision grid @node+436; a node binding it = breakable/destructible
	hide_hud = false, no_cull = false, cam_ortho = false, deint_offset = false,
	cam_hscale_on = false, cam_hscale = 0.75, cam_zoom = 1.0, cam_link_zoom = true, ortho_dist = 20.0,
	snap_ortho = true, -- axis-snap (Freecam pane) also flips on ortho for a clean 2D-style view
	snap_pivot = 0, -- freecam snap/orbit pivot: 0 midpoint / 1 P1 / 2 P2 / 3 stage origin
	fc_follow = false, -- freecam continuously re-aims at the snap pivot as it moves
	show_gizmo = true, -- floating clickable view-axis gizmo (top-right) while freecam is on
	force_stage = false, stage_id = 10, -- pin g_SelectedStageId@0x502158 (1..12) to force the battle stage (all modes)
	fc_fix_cull = true, cull_expand = 2.5,
	fc_move_speed = 3.0, fc_look_speed = 0.03, fc_mouse_sens = 0.0030, fc_look_smooth = 0.55, fc_invert_y = false,
	frame_delay = 2, -- +N nudge atop the auto-aligned (EE-thread capture + FIFO) overlay; 0 = engine baseline, 2 = default

	skel_thickness = 1.5, box_thickness = 2.0, sphere_segments = 20, sphere_shaded = true, sphere_fill_alpha = 0.5,
	-- colors as {r,g,b,a} in 0..1
	col_hurt    = { 0.24, 0.86, 0.35, 0.70 },
	col_attack  = { 1.00, 0.24, 0.24, 0.90 },
	col_atk_hi  = { 1.00, 0.59, 0.16, 0.90 },
	col_atk_lo  = { 0.94, 0.88, 0.22, 0.90 },
	col_throw   = { 0.86, 0.27, 0.86, 0.86 },
	col_cmdgrab = { 1.00, 0.47, 0.80, 0.85 },
	col_prox    = { 1.00, 0.78, 0.16, 0.74 },
	col_body    = { 0.35, 0.65, 1.00, 0.72 },
	col_clash   = { 0.65, 0.65, 0.69, 0.66 },
	col_special = { 0.31, 0.90, 0.82, 0.80 },
	col_skel    = { 0.47, 0.90, 0.47, 0.78 },
	col_shell   = { 1.00, 0.50, 0.10, 0.85 },
	col_stage_floor = { 0.30, 0.72, 1.00, 0.55 }, -- stage collision: floor planes (cool blue)
	col_stage_wall  = { 1.00, 0.45, 0.22, 0.62 }, -- out-of-bounds / ring walls (orange)
	col_stage_obst  = { 0.78, 1.00, 0.30, 0.66 }, -- breakable obstacles (lime)
}

-- snapshot the defaults (for the "Defaults" button) BEFORE merging the saved file over cfg
local DEFAULTS = {}
for k, v in pairs(cfg) do DEFAULTS[k] = (type(v) == "table") and { v[1], v[2], v[3], v[4] } or v end

local function save_cfg() persist.save(CFG_PATH, cfg) end
local function reset_defaults()
	for k, v in pairs(DEFAULTS) do cfg[k] = (type(v) == "table") and { v[1], v[2], v[3], v[4] } or v end
	cfg.show_window = true -- keep the panel visible after a reset
	save_cfg()
end

persist.load(CFG_PATH, cfg)
local ui = require("ui").bind(cfg, save_cfg) -- imgui widgets bound to cfg[...] + autosave

-- ============================ control-window helpers (shared with module panes via ctx) ============================
local popout_on = false -- render the panel in a separate OS window (runtime-only; not persisted)
local function pane(label, key) -- collapsible header; persists its open state in cfg[key]
	-- `or false`: CollapsingHeader's 2nd arg must be a boolean -- a key missing from cfg defaults (new pane) would
	-- pass nil and crash the C++ binding ("expected boolean, received nil"). Default any unknown pane to closed.
	local open = imgui.CollapsingHeader(label, cfg[key] or false)
	if open ~= cfg[key] then cfg[key] = open; save_cfg() end
	return open
end
local function row(label, show_key, col_key) -- checkbox + compact colour swatch on one line
	ui.checkbox(label, show_key); imgui.SameLine(168)
	ui.color_row("##c" .. label, col_key)
end
local function crow(label, col_key) -- indented colour-only sub-row (visibility follows its parent group)
	imgui.Indent(16); imgui.Text(label); imgui.Unindent(16); imgui.SameLine(168)
	ui.color_row("##c" .. label, col_key)
end

-- ============================ modules ============================
local boxes   = require("sdbz.boxes")
local hud     = require("sdbz.hud")
local camera  = require("sdbz.camera")
local train   = require("sdbz.train")
local patches = require("sdbz.patches")
local rollbk  = require("sdbz.rollback")

local ctx = { cfg = cfg, save = save_cfg, reset_defaults = reset_defaults, col = col, ui = ui,
	pane = pane, row = row, crow = crow }
boxes.init(ctx); hud.init(ctx); camera.init(ctx); train.init(ctx); patches.init(ctx); rollbk.init(ctx)

-- ============================ control window ============================
-- Drawn from on_gui() (separate from on_frame's world drawing) so the host can render it in-game OR in the
-- pop-out OS window. A top row + collapsible panes (each module draws its own pane); the small engine-config
-- panes (aspect / deinterlace / style / frame-delay) stay inline here.
local function control_window()
	if not cfg.show_window then return end
	-- controller_nav off (default) -> NoNav window: the gamepad ignores the panel and keeps driving the game.
	-- When popped out, the host fills the OS window for us -> drop the title bar / chrome (popout_on = fill).
	-- restore the saved window position/size ONCE per session (gpbear: "save lua window size/position");
	-- afterwards the user drags/resizes freely and the live values are tracked below -> persisted by Save.
	if not popout_on and cfg.win_w and cfg.win_w > 60 then
		imgui.SetNextWindowPos(cfg.win_x or 40, cfg.win_y or 40, true)
		imgui.SetNextWindowSize(cfg.win_w, cfg.win_h or 400, true)
	end
	if imgui.Begin("SDBZ Hitboxes (Lua)", cfg.controller_nav, popout_on) then
		-- track the live window geometry (skip the popout -- the host sizes that window, not us)
		if not popout_on then
			local wx, wy = imgui.GetWindowPos()
			local ww, wh = imgui.GetWindowSize()
			cfg.win_x, cfg.win_y, cfg.win_w, cfg.win_h = wx, wy, ww, wh
		end
		-- Top row (always visible): master draw gate + settings persistence.
		ui.checkbox("Master", "master"); imgui.SameLine()
		if imgui.Button("Save") then save_cfg() end; imgui.SameLine()
		if imgui.Button("Reload") then persist.load(CFG_PATH, cfg) end; imgui.SameLine()
		if imgui.Button("Defaults") then reset_defaults() end
		imgui.SameLine(); local pch, pv = imgui.Checkbox("Pop out", popout_on); if pch then popout_on = pv end
		imgui.SetItemTooltip("Render this panel in a separate OS window you can drag to another monitor.")
		ui.checkbox("Controller navigation (gamepad can move this menu)", "controller_nav")
		imgui.SetItemTooltip("Off (default): the gamepad ignores this panel and keeps controlling the game.\nOn: the pad can navigate the menu (and will also drive the game).")

		boxes.pane()
		train.step_pane()
		train.train_pane()
		rollbk.pane()
		camera.freecam_pane()
		camera.cam_pane()

		if pane("Aspect ratio (display)", "p_aspect") then
			local aspNames = { "Stretch (fill)", "Auto 4:3/3:2", "4:3 (PS2 hardware)", "16:9", "10:7 (true pixels)", "8:7" }
			local a = engine.get_aspect(); if a < 0 or a > 5 then a = 2 end
			local ch; ch, a = imgui.Combo("Aspect", a, aspNames); if ch then engine.set_aspect(a) end
			if imgui.Button("4:3 (PS2 hardware)") then engine.set_aspect(2); cfg.cam_hscale_on = false; save_cfg() end
			imgui.SameLine()
			if imgui.Button("10:7 (2D correct)") then engine.set_aspect(4); cfg.cam_hscale_on = true; cfg.cam_hscale = 0.70; cfg.cam_link_zoom = false; cfg.cam_zoom = 1.0; save_cfg() end
			imgui.SameLine()
			if imgui.Button("Widescreen 16:9") then engine.set_aspect(0); cfg.cam_hscale_on = true; cfg.cam_hscale = 0.46875; cfg.cam_link_zoom = false; cfg.cam_zoom = 1.0; save_cfg() end
			imgui.TextDisabled("FB 640x448 (10:7) stretched to 4:3 by the DISPLAY reg. '10:7' = 2D undistorted + 3D h-scale 0.70.")
			local fbw, fbh = memory.read_u32(0x503100), memory.read_u32(0x503104)
			local magh, vmode = memory.read_u32(0x503110), memory.read_u32(0x5030F4)
			if fbw > 0 and fbw <= 1024 and fbh > 0 and fbh <= 1024 then
				imgui.Text(string.format("Live: FB %dx%d  grid %.3f  MAGH=%d  mode=%d", fbw, fbh, fbw / fbh, magh, vmode))
			end
		end

		if pane("Deinterlace", "p_deint") then
			-- The half-line jitter is the GAME shifting the GS DISPLAY reg by a field each frame (g_GsInterlaceField
			-- @0x503154, read from GS_CSR bit13). PCSX2's GS interlace settings can't cancel it -- the real fix is to
			-- pin the game's field to 0, which we do by patching the field-poll store @0x104C68 (sw $v0 -> sw $zero),
			-- exactly like the "No-Interlacing" pnach. No GS "mode" knobs needed.
			ui.checkbox("Disable interlace offset (stable image)", "deint_offset")
			imgui.TextDisabled("Pins the game's interlace field to 0 (code patch @0x104C68) so the picture stops bobbing a\nhalf-line every frame. Pair with Freeze for clean frame-stepping. SLUS-214.42 only for now.")
		end

		if pane("Style", "p_style") then
			ui.slider_float("Box thickness", "box_thickness", 0.5, 4.0, "%.1f")
			ui.slider_float("Skeleton thickness", "skel_thickness", 0.5, 4.0, "%.1f")
			ui.slider_int("Sphere detail", "sphere_segments", 6, 40, "%d")
			ui.checkbox("Shaded fill", "sphere_shaded"); imgui.SameLine()
			imgui.SetNextItemWidth(120)
			ui.slider_float("##fillA", "sphere_fill_alpha", 0.05, 1.0, "fill %.2f")
		end

		if pane("Frame delay", "p_frame") then
			ui.slider_int("##delay", "frame_delay", 0, 8, "delay = %d frames")
			imgui.TextDisabled("The overlay now AUTO-ALIGNS: geometry is read on the EE thread and delivered to the\nrenderer in frame order, so 0 is already frame-perfect. This only nudges it +N frames\nif a particular VSync / sync-to-host setup ever needs it.")
		end

		if pane("Stage (force)", "p_stage") then
			-- pins g_SelectedStageId@0x502158 (the id the stage loader reads at battle start, every mode). Beats
			-- Training's one-shot 0x0A force; also lets you pick any stage in any mode. Loads on the NEXT stage load.
			-- Stage factory maps id 0..12 -> CStage00..CStage12 (13+ falls back to CStage00). Menus only expose
			-- 1..12, so id 0 = CStage00 is the unlisted one. Combo index == stage id here (0-based incl. CStage00).
			local stageNames = { "0  s00 -- UNLISTED stage (try it!)", "1  Wasteland (Day)", "2  E. Capital (Day)", "3  Kami's Lookout",
				"4  Enma's Palace", "5  Namek", "6  Budokai Ring", "7  Cell Ring", "8  Wasteland (Night)",
				"9  E. Capital (Evening)", "10  Vegeta's Capsule (training)", "11  Wasteland (no clouds) [buggy]",
				"12  King Kai's Planet" }
			ui.checkbox("Force stage (all modes incl. Training)", "force_stage")
			cfg.stage_id = cfg.stage_id or 10
			-- any change writes g_SelectedStageId immediately (so the field "overwrites" right away; the live readout
			-- updates) -- the stage itself still rebuilds on the NEXT battle load. Force-stage pins it every frame.
			local function set_stage(id)
				cfg.stage_id = (id < 0) and 0 or (id > 255 and 255 or id); save_cfg()
				memory.write_u8(0x502158, cfg.stage_id)
			end
			if cfg.stage_id >= 0 and cfg.stage_id <= 12 then
				local ch, ni = imgui.Combo("Stage", cfg.stage_id, stageNames); if ch then set_stage(ni) end
			else
				imgui.TextDisabled(string.format("Stage id %d -- beyond the factory (13+ => CStage00). Probing.", cfg.stage_id))
			end
			-- RAW ID box: type ANY byte (0..255) to probe. The +/- buttons step it; the value writes on commit.
			imgui.SetNextItemWidth(120)
			local ich, iv = imgui.InputInt("Stage id (raw)", cfg.stage_id, 1); if ich then set_stage(iv) end
			imgui.SameLine(); imgui.Text(string.format("live = %d", memory.read_u32(0x502158) & 0xFF))
			imgui.TextDisabled("id 0 = stg/s00, a REAL stage no menu offers -- worth a look. Only 13 stages exist (s00..s12).\nSets the id now; the stage rebuilds on the NEXT battle load (restart / re-enter). 11 buggy. Force = pin.")
		end
	end
	imgui.End()
end

-- ============================ entry points ============================
-- on_capture: GEOMETRY ONLY. The engine runs this on the EE/CPU thread at end-of-frame, so the boxes are read
-- from the EXACT frame about to be displayed -> frame-perfect (no jitter, no spheres at the wrong spot, no fixed
-- delay to guess). MUST stay pure: memory reads + project.* + draw.* (which only EMIT prims). NO imgui / input /
-- engine bookkeeping here -- those touch GS-thread state and live in on_frame below.
function on_capture()
	if not cfg.master then return end
	boxes.draw_world()
end

-- on_frame: BOOKKEEPING + the stats HUD. Runs on the GS thread each present. Engine config + the game-state
-- hacks (training/patches/camera/freecam/freeze), plus the text HUD (which uses imgui text metrics -> GS-thread
-- only). The HUD reads stats from the EE thread's slightly-ahead state, which is fine -- it's text, not a hitbox.
function on_frame()
	engine.set_cursor(cfg.show_window)         -- keep the GS thread live + draw a cursor while our window is open
	engine.set_gamepad_nav(cfg.controller_nav) -- OFF by default -> the pad drives the game, not our menu
	engine.set_frame_delay(cfg.frame_delay)    -- optional +N nudge into the presented-frame history (0 = auto-aligned)
	engine.set_popout(popout_on)               -- render on_gui() in a separate OS window when checked
	train.training_update()
	patches.update()
	if cfg.force_stage then memory.write_u8(0x502158, cfg.stage_id) end -- pin g_SelectedStageId; the loader reads it at stage load (any mode)
	camera.cam_scale_update()
	camera.ortho_update()
	camera.freecam_update()
	camera.gizmo_update() -- floating view-axis gizmo (screen-space; after freecam so it reflects the current view)
	engine.claim_mouse(camera.gizmo_hovering()) -- cursor over the gizmo -> suppress PCSX2's double-click fullscreen
	train.freeze_update()
	train.speed_update() -- game-speed slider: re-write the engine time-scale (no-op while frozen/off)
	hud.draw() -- stats HUD (text). On the GS thread so imgui text metrics are valid.
end

-- on_gui: the ImGui control panel. The host calls this in the in-game context (default) OR, when "Pop out" is
-- ticked, in the separate pop-out OS window's context. Keep it to imgui.* only -- world drawing lives in on_frame.
function on_gui()
	control_window()
end

-- rebindable hotkeys routed from PCSX2 Settings->Hotkeys ("Script Overlay" category). The host queues the
-- press on whatever thread fires it and drains it on the GS thread right before on_frame, so these run in the
-- same context as everything else (safe to touch EE RAM / patches).
function on_hotkey(name)
	if name == "toggle_freeze" then train.freeze_set(not train.frozen())
	elseif name == "frame_step" then train.freeze_step(1)
	elseif name == "toggle_freecam" then camera.freecam_set_enabled(not camera.freecam_enabled())
	elseif name == "toggle_window" then cfg.show_window = not cfg.show_window; save_cfg()
	elseif name == "toggle_master" then cfg.master = not cfg.master; save_cfg()
	end
end
