-- SDBZ hitbox / hurtbox / skeleton / projectile drawing + the "Box types & colors" control pane.
-- Geometry only -- everything here is called from on_capture (EE thread, frame-perfect). init(ctx) wires
-- cfg / col / ui / pane-helpers; bone + player helpers come from sdbz.players.
local P = require("sdbz.players")
local valid = P.valid

local M = {}
local cfg, col, ui, pane, row, crow, save -- set by init()

function M.init(ctx)
	cfg, col, ui = ctx.cfg, ctx.col, ctx.ui
	pane, row, crow = ctx.pane, ctx.row, ctx.crow
	save = ctx.save
end

-- ============================ skeleton ============================
-- Full-rig skeleton from the engine's AUTHORITATIVE bone-node array (skel = *(player+1968); array @skel+0x0C;
-- 97 slots, ctor CSkeleton_ctor@0x1D6950 zeroes bone idx 0..96). That array is a MIX of three things, all
-- verified live against the model the working RAM-skin tool reads:
--   1. real skinning bones   -- node+440 == the shared model/skeleton object (the "modelobj")
--   2. group/coordinate/mesh helper nodes -- node+440 is 0 or garbage (and +436 is a junk index like 0x90009)
--   3. real bones the engine DID NOT pose this frame -- left at the model-local origin (~0,15,0) with an
--      IDENTITY world 3x3; left in, each draws a long line from the character to origin.
-- A drawable bone must (a) share the modelobj at +440 AND (b) be marked posed via node+40 bit 0. Then connect
-- each bone to its nearest ancestor bone via the real node-tree parent (node+12). No Y/length/distance
-- heuristics -- both tests are exact structural facts from the engine's own data.
local function draw_skeleton(player)
	if not cfg.skel then return end
	local skel = memory.read_u32(player + 1968)
	if not valid(skel) then return end
	-- read the array once; cache each live node's +440
	local nodes, m440 = {}, {}
	for i = 0, 96 do
		local node = memory.read_u32(skel + 12 + 4 * i)
		if valid(node) then nodes[#nodes + 1] = node; m440[node] = memory.read_u32(node + 440) end
	end
	-- modelobj = the +440 the real bones share (mode over entries with a sane bone index @+436; the helpers
	-- carry a junk +436 >= 200 so they don't pollute the vote).
	local counts, mo, best = {}, 0, -1
	for _, node in ipairs(nodes) do
		local m = m440[node]
		if valid(m) and memory.read_u32(node + 436) < 200 then
			counts[m] = (counts[m] or 0) + 1
			if counts[m] > best then best = counts[m]; mo = m end
		end
	end
	if not valid(mo) then return end
	-- keep real bones (share modelobj) that the engine actually POSES. node+40 = scene-node flags; bit 0 is the
	-- "live/posed" bit -- SET (0x1007) on every animated skeleton bone, CLEAR (0x1004) on the ShapeHead* morph/
	-- blend-shape bones the engine never poses (those stay at the model-local origin and otherwise draw a long
	-- line from the character out across the stage). Verified live on both players: bit 0 separates ALL posed
	-- bones from ALL morph bones, and unlike the world-matrix 3x3 it is STATE-INDEPENDENT (a morph bone keeps
	-- bit 0 clear even when its rotation is non-identity -- which is why the earlier identity test failed).
	local is_bone = {}
	for _, node in ipairs(nodes) do
		if m440[node] == mo and (memory.read_u32(node + 40) & 0x1) ~= 0 then is_bone[node] = true end
	end
	local scol = col(cfg.col_skel)
	for node in pairs(is_bone) do
		-- nearest ANCESTOR bone via the real node-tree parent (skips non-bone group/coordinate nodes)
		local par = memory.read_u32(node + 12)
		local g = 0
		while valid(par) and not is_bone[par] and g < 64 do par = memory.read_u32(par + 12); g = g + 1 end
		if is_bone[par] then
			-- world translation = row 3 of the world 4x4 (node+240) -> node+288/292/296
			draw.line_world(
				memory.read_f32(node + 288), memory.read_f32(node + 292), memory.read_f32(node + 296),
				memory.read_f32(par + 288), memory.read_f32(par + 292), memory.read_f32(par + 296),
				scol, cfg.skel_thickness)
		end
	end
end

-- ============================ hurt + active boxes ============================
local function draw_hurt(player, skel)
	if not cfg.hurt then return end
	local tbase = 0
	for b = 0, 8 do
		local node = memory.read_u32(player + 6056 + 4 * b)
		if valid(node) then
			local box = memory.read_u32(node + 20)
			if valid(box) and (tbase == 0 or box < tbase) then tbase = box end
		end
	end
	if not valid(tbase) then return end
	local c = col(cfg.col_hurt)
	for k = 0, 23 do
		local a = tbase + 32 * k
		local r = memory.read_f32(a + 16)
		if not (r > 0.0) then break end
		P.bone_sphere(skel, memory.read_u32(a + 20),
			memory.read_f32(a), memory.read_f32(a + 4), memory.read_f32(a + 8), r, c)
	end
end

local function draw_entry(skel, he, c, label, frame)
	local s = memory.read_u16(he + 0x20)
	local e = memory.read_u16(he + 0x22)
	if frame < s or frame > e then return end
	local have = false
	for j = 0, 7 do
		local bp = memory.read_u32(he + 0x24 + 4 * j)
		if valid(bp) then
			P.bone_sphere(skel, memory.read_u32(bp + 20),
				memory.read_f32(bp), memory.read_f32(bp + 4), memory.read_f32(bp + 8), memory.read_f32(bp + 16), c)
			if (not have) and label ~= "" and cfg.labels then
				local sx, sy = P.box_screen(skel, bp)
				if sx then draw.text(sx, sy, c, label); have = true end
			end
		end
	end
end

local function draw_active(player, skel)
	local grp = memory.read_u32(player + 6204)
	local cm  = memory.read_u32(player + 1976)
	if not valid(grp) or not valid(cm) then return end
	local frame = memory.read_u32(cm + 152)
	if frame >= 0x80000000 then frame = frame - 0x100000000 end

	for i = 0, 15 do
		local he = memory.read_u32(grp + 8 + 4 * i)
		if valid(he) then
			local mask = memory.read_u32(he + 0x1c)
			local c, lbl, show
			if (mask & 0x480) ~= 0 then
				c, lbl, show = col(cfg.col_throw), "THROW", cfg.throw
			elseif (mask & 0x100) ~= 0 then
				c, lbl, show = col(cfg.col_cmdgrab), "CMD-GRAB", cfg.throw
			elseif (mask & 0x1000000) ~= 0 then
				c, lbl, show = col(cfg.col_body), "BODY", cfg.other
			elseif (mask & 0x8000000) ~= 0 then
				c, lbl, show = col(cfg.col_clash), "CLASH", cfg.other
			elseif (mask & 0x800) ~= 0 then
				c, lbl, show = col(cfg.col_special), "SPECIAL", cfg.other
			else
				local gh = mask & 0x60000000
				local ghs = (gh == 0x40000000) and "-OH" or (gh == 0x20000000) and "-LO" or (gh == 0x60000000) and "-MD" or ""
				c = (gh == 0x40000000) and col(cfg.col_atk_hi)
					or (gh == 0x20000000) and col(cfg.col_atk_lo)
					or col(cfg.col_attack)
				lbl = "ATK" .. ghs .. (((mask & 0x1) ~= 0) and "" or "*UB") .. (((mask & 0x40) ~= 0) and "*ND" or "")
				show = cfg.attack
			end
			if show then draw_entry(skel, he, c, lbl, frame) end
		end
	end

	if cfg.prox then
		for i = 0, 7 do
			local he = memory.read_u32(grp + 0x68 + 4 * i)
			if valid(he) then draw_entry(skel, he, col(cfg.col_prox), "PROX", frame) end
		end
	end
end

-- ============================ projectile / shell capsules (world-space) ============================
local function isnan(x) return x ~= x end

local function draw_shells(player)
	if not cfg.shells then return end
	local scol = col(cfg.col_shell)
	for i = 0, 15 do
		local shell = memory.read_u32(player + 1820 + 4 * i)
		if valid(shell) and (memory.read_u32(shell + 832) & 8) ~= 0
			and (memory.read_u32(shell + 1440) & 0x100) ~= 0 then       -- 0x100 = hitbox ACTIVE this frame
			local ho = memory.read_u32(shell + 1384)
			local segbuf = memory.read_u32(shell + 1376)
			local parbuf = memory.read_u32(shell + 1388)
			if valid(segbuf) then
				local count
				if valid(ho) then
					count = memory.read_u32(ho + 12)
				else
					local c = (memory.read_u32(shell + 1380) >> 8) & 0xFF
					count = (c >= 1 and c <= 64) and c or 1
				end
				if count < 1 then count = 1 elseif count > 64 then count = 64 end
				for k = 0, count - 1 do
					local seg = segbuf + 48 * k
					local bx, by, bz = memory.read_f32(seg + 16), memory.read_f32(seg + 20), memory.read_f32(seg + 24)
					local ax, ay, az = memory.read_f32(seg), memory.read_f32(seg + 4), memory.read_f32(seg + 8)
					if not (isnan(bx) or isnan(by) or isnan(bz) or isnan(ax)) then
						local aOK = (ax * ax + ay * ay + az * az) >= 1.0
						local bOK = (bx * bx + by * by + bz * bz) >= 1.0
						if aOK or bOK then
							local r = 1.5
							if valid(parbuf) then
								local pr = memory.read_f32(parbuf + 32 * k + 16)
								if pr > 0.05 and pr < 300.0 then r = pr end
							else
								local sr = memory.read_f32(seg + 40)
								if sr > 0.05 and sr < 300.0 then r = sr end
							end
							if bOK then draw.sphere_world(bx, by, bz, r, scol) end
							if aOK and bOK then draw.line_world(ax, ay, az, bx, by, bz, scol, cfg.box_thickness) end
						end
					end
				end
			end
		end
	end
end

-- ============================ stage collision geometry (world-space) ============================
-- The stage's collision is a tree of CStageCollNode scene nodes (vtable 0x4E7530), each carrying a WORLD 4x4
-- @node+240 and a linked list of CHitMesh primitives (head@node+440, next@mesh+176, 0 ends). Per mesh: category
-- mask@+196 (0x100000=floor, 0x180000=wall/out-of-bounds, others=breakable/sub), radius@+204, bounding CENTER@+208
-- (node-local vec3) and HALF-EXTENTS@+224 (node-local vec3; .Y==0 => a flat FLOOR plane, else a solid BOX). World
-- geometry = node world matrix applied to (center +/- half-extents). RE'd + live-verified 2026-06-20 (Capsule floor
-- cell Hitba00 -> world Y 16.7 vs standing player 16.0). Walk from the STAGE scene group = *(stage+0x68);
-- stage = *( *( *(0x63FEAC) + 104 ) + 104 ). Collision nodes are detected vtable-agnostically by the CHitMesh
-- owner back-ref (mesh+180 == node) -- CHitMesh has several subclass vtables (plane/box/tri), so a fixed node
-- vtable filter missed wall/ceiling nodes. See sdbz-stage-collision memory note.

local function draw_face(mat, pool, start, nv, c) -- nv node-local verts (stride16, vec4) -> world; closed polygon
	local fx, fy, fz, px, py, pz
	for k = 0, nv - 1 do
		local v = pool + 16 * (start + k)
		local wx, wy, wz = project.apply_matrix_at(mat,
			memory.read_f32(v), memory.read_f32(v + 4), memory.read_f32(v + 8))
		if k == 0 then fx, fy, fz = wx, wy, wz
		else draw.line_world(px, py, pz, wx, wy, wz, c, cfg.box_thickness) end
		px, py, pz = wx, wy, wz
	end
	if fx and nv > 2 then draw.line_world(px, py, pz, fx, fy, fz, c, cfg.box_thickness) end
end

-- Draw a CHitMesh as its ACTUAL collision FACES (polygons). mesh+12 = face count; mesh+20 = face-record array
-- (stride 32: dword[0]=vertex count, dword[1]=start index into the vertex pool, dword[4..7]=face normal vec4);
-- mesh+16 = vertex pool (stride 16 vec4, node-local) -> world via node matrix@+240. Colour each face by its normal:
-- |ny|>=0.6 => floor/ceiling, else wall. Sphere fallback (no face data) = center@+208 + radius@+204. NOTE: this is
-- the per-frame Lua path; the fast path uses the native draw.collision_mesh batch when the host provides it.
local function draw_coll_mesh(mat, mesh, cf, cw, co)
	local count = memory.read_u32(mesh + 12)
	local recs  = memory.read_u32(mesh + 20)
	local pool  = memory.read_u32(mesh + 16)
	if count >= 1 and count <= 8192 and valid(recs) and valid(pool) then
		for f = 0, count - 1 do
			local rec = recs + 32 * f
			local nv = memory.read_u32(rec)
			if nv >= 2 and nv <= 64 then
				local ny = memory.read_f32(rec + 20) -- face normal Y (dword[5])
				if ny >= 0.6 or ny <= -0.6 then
					if cfg.stage_floor then draw_face(mat, pool, memory.read_u32(rec + 4), nv, cf) end
				elseif cfg.stage_walls then
					draw_face(mat, pool, memory.read_u32(rec + 4), nv, cw)
				end
			end
		end
	elseif cfg.stage_obst then -- no face data => fall back to a sphere
		local r = memory.read_f32(mesh + 204)
		if r > 0.0 then
			local wx, wy, wz = project.apply_matrix_at(mat,
				memory.read_f32(mesh + 208), memory.read_f32(mesh + 212), memory.read_f32(mesh + 216))
			draw.sphere_world(wx, wy, wz, r, co)
		end
	end
end

local function node_name(node) -- inline char[] @node+20 (<=12, null-terminated)
	local t = {}
	for i = 0, 11 do
		local b = memory.read_u8(node + 20 + i)
		if b == 0 then break end
		t[#t + 1] = string.char(b)
	end
	return table.concat(t)
end

-- A node owns collision iff its mesh-list head @node+440 is a CHitMesh whose owner back-ptr @mesh+180 points back at
-- it. vtable-agnostic, and it naturally skips CStageModel nodes (their +440 is a modelobj whose +180 isn't the node).
local function coll_head(node)
	local m = memory.read_u32(node + 440)
	if valid(m) and memory.read_u32(m + 180) == node then return m end
	return 0
end

-- visit every collision node in the live STAGE subtree: fn(node, headMesh). DFS child@+16 / sibling@+8,
-- seen-set + budget guard against cycles/runaway. (Name is read by callers only when needed -- skipped on the hot
-- draw path unless something is hidden.)
local function each_coll(fn)
	local h = memory.read_u32(0x63FEAC); if not valid(h) then return end
	local mgr = memory.read_u32(h + 104); if not valid(mgr) then return end
	local stg = memory.read_u32(mgr + 104); if not valid(stg) then return end
	local root = memory.read_u32(stg + 0x68); if not valid(root) then return end -- STAGE scene group
	local stack, top, seen, budget = { root }, 1, {}, 8000
	while top >= 1 and budget > 0 do
		local node = stack[top]; stack[top] = nil; top = top - 1; budget = budget - 1
		if valid(node) and not seen[node] then
			seen[node] = true
			local hm = coll_head(node)
			if hm ~= 0 then fn(node, hm) end
			local ch = memory.read_u32(node + 16); if valid(ch) then top = top + 1; stack[top] = ch end
			local sb = memory.read_u32(node + 8);  if valid(sb) then top = top + 1; stack[top] = sb end
		end
	end
end

local function hidden(name) -- comma-set membership in cfg.stage_hidden
	local h = cfg.stage_hidden
	if not h or h == "" then return false end
	return ("," .. h .. ","):find("," .. name .. ",", 1, true) ~= nil
end

-- Live every frame (no caching -> destructible/moving collision stays correct). FAST PATH: draw.collision_tree does
-- the ENTIRE stage in one native call -- DFS-walks the scene tree, transforms/projects/emits every collision face in
-- C++, dedups shared edges, caches vertex projections (the ~600 per-mesh Lua dispatches + ~8000 redundant lines were
-- the FPS sink). All SDBZ offsets are passed in (game knowledge stays here). Per-category colour 0 = toggled off.
-- Resolve the STAGE scene group = *(stage+0x68); stage = *( *( *(0x63FEAC)+104 )+104 ). Falls back to the per-mesh
-- Lua path (draw.collision_mesh / draw_coll_mesh) on a host without the native binding.
local STAGE_OFFS = {
	child_off = 16, sib_off = 8, mat_off = 240, name_off = 20, grid_off = 436, -- scene node (+436 = bound CHitGrid)
	head_off = 440, owner_off = 180, next_off = 176,           -- collision node -> CHitMesh list
	count_off = 12, pool_off = 16, recs_off = 20, center_off = 208, radius_off = 204, -- CHitMesh
	rec_stride = 32, vcount_off = 0, start_off = 4, normal_off = 20,                  -- face record
}
local function stage_root()
	local h = memory.read_u32(0x63FEAC); if not valid(h) then return 0 end
	local mgr = memory.read_u32(h + 104); if not valid(mgr) then return 0 end
	local stg = memory.read_u32(mgr + 104); if not valid(stg) then return 0 end
	local root = memory.read_u32(stg + 0x68); return valid(root) and root or 0
end

local function draw_stage()
	if not cfg.stage then return end
	local root = stage_root(); if root == 0 then return end
	if draw.collision_tree then -- fast path: whole stage in one native call
		STAGE_OFFS.root = root
		STAGE_OFFS.thick = cfg.box_thickness
		STAGE_OFFS.ceil = 0.6
		STAGE_OFFS.hidden = cfg.stage_hidden or ""
		STAGE_OFFS.brk_grid = cfg.stage_brk_grid or 0 -- node+436 == this => breakable: draw it in the obstacle colour
		STAGE_OFFS.floor = cfg.stage_floor and col(cfg.col_stage_floor) or 0
		STAGE_OFFS.wall  = cfg.stage_walls and col(cfg.col_stage_wall) or 0
		STAGE_OFFS.obst  = cfg.stage_obst  and col(cfg.col_stage_obst) or 0
		draw.collision_tree(STAGE_OFFS)
		return
	end
	-- fallback: per-mesh Lua walk (slower; host without the native tree binding)
	local rf, rw, ro = col(cfg.col_stage_floor), col(cfg.col_stage_wall), col(cfg.col_stage_obst)
	local native = draw.collision_mesh
	local cf = (native and cfg.stage_floor) and rf or 0
	local cw = (native and cfg.stage_walls) and rw or 0
	local checkhide = (cfg.stage_hidden or "") ~= ""
	each_coll(function(node, head)
		if checkhide and hidden(node_name(node)) then return end
		local mat, m, g = node + 240, head, 0
		while valid(m) and g < 256 do
			if native then
				local count = memory.read_u32(m + 12)
				local recs  = memory.read_u32(m + 20)
				local pool  = memory.read_u32(m + 16)
				if count >= 1 and count <= 8192 and valid(recs) and valid(pool) then
					draw.collision_mesh(mat, pool, recs, count, 32, 0, 4, 20, cf, cw, 0.6, cfg.box_thickness)
				elseif cfg.stage_obst then
					local r = memory.read_f32(m + 204)
					if r > 0.0 then
						local wx, wy, wz = project.apply_matrix_at(mat,
							memory.read_f32(m + 208), memory.read_f32(m + 212), memory.read_f32(m + 216))
						draw.sphere_world(wx, wy, wz, r, ro)
					end
				end
			else
				draw_coll_mesh(mat, m, rf, rw, ro)
			end
			m = memory.read_u32(m + 176); g = g + 1
		end
	end)
end

-- per-element list for the control pane: aggregate collision nodes by name -> mesh count (walks live; called from
-- M.pane on the GUI thread while the Stage section is open).
function M.stage_elements()
	local agg, order, grids, glist = {}, {}, {}, {}
	each_coll(function(node, head)
		local name = node_name(node)
		if name == "" then name = "(unnamed)" end
		-- DIAGNOSTIC: each collision node binds a CHitGrid at node+436; breakable nodes bind the area's index-1 grid.
		-- Tally distinct grids (value + grid+12) so we can see how breakable-ness clusters + ground-truth it on a break.
		local grid = memory.read_u32(node + 436)
		local gd = grids[grid]
		if not gd then
			gd = { grid = grid, count = 0, sample = name, g12 = valid(grid) and memory.read_u32(grid + 12) or 0 }
			grids[grid] = gd; glist[#glist + 1] = gd
		end
		gd.count = gd.count + 1
		local e = agg[name]
		if not e then e = { name = name, meshes = 0, faces = 0, edges = 0, grid = grid }; agg[name] = e; order[#order + 1] = name end
		local m, g = head, 0
		while valid(m) and g < 256 do
			e.meshes = e.meshes + 1
			local fc = memory.read_u32(m + 12)
			local recs = memory.read_u32(m + 20)
			if fc >= 1 and fc <= 8192 and valid(recs) then
				e.faces = e.faces + fc
				for f = 0, fc - 1 do
					local nv = memory.read_u32(recs + 32 * f)
					if nv >= 2 and nv <= 64 then e.edges = e.edges + nv end -- nv edges per closed polygon
				end
			end
			m = memory.read_u32(m + 176); g = g + 1
		end
	end)
	table.sort(order)
	table.sort(glist, function(a, b) return a.count > b.count end)
	return agg, order, glist
end

function M.set_hidden(name, hide) -- toggle a node-name in the comma-set cfg.stage_hidden (+ persist)
	local parts = {}
	for p in (cfg.stage_hidden or ""):gmatch("[^,]+") do
		if p ~= name then parts[#parts + 1] = p end
	end
	if hide then parts[#parts + 1] = name end
	cfg.stage_hidden = table.concat(parts, ",")
	if save then save() end
end

M.hidden = hidden -- exposed so the pane's checkbox list can show current visibility

-- ============================ public: capture-time world drawing ============================
-- Called from on_capture (EE thread). Sets the world->screen matrix + sphere style, then draws every box
-- type for both players. Pure: memory reads + project.* + draw.* (no imgui / input).
function M.draw_world()
	project.set_world_matrix(P.WS_MATRIX)
	draw.set_sphere_style(cfg.sphere_segments, cfg.sphere_shaded, cfg.sphere_fill_alpha, cfg.box_thickness)
	draw_stage() -- stage collision volumes (once; not per-player)
	for i = 0, 1 do
		local p = P.get_player(i)
		if p ~= 0 and P.player_valid(p) then
			local skel = memory.read_u32(p + 1968)
			if valid(skel) then
				draw_skeleton(p)
				draw_hurt(p, skel)
				draw_active(p, skel)
				draw_shells(p)
			end
		end
	end
end

-- ============================ control pane ============================
function M.pane()
	if not pane("Box types & colors", "p_boxes") then return end
	ui.checkbox("Labels (guard height / category)", "labels")
	row("Hurt (body)", "hurt", "col_hurt")
	row("Attack (mid)", "attack", "col_attack")
	crow("overhead (OH)", "col_atk_hi")
	crow("low (LO)", "col_atk_lo")
	row("Throw / grab", "throw", "col_throw")
	crow("command grab", "col_cmdgrab")
	ui.checkbox("Other volumes", "other")
	crow("body / push", "col_body")
	crow("clash / collision", "col_clash")
	crow("special", "col_special")
	row("Proximity", "prox", "col_prox")
	row("Projectiles", "shells", "col_shell")
	row("Skeleton", "skel", "col_skel")
	ui.checkbox("Stage collision geometry", "stage")
	imgui.SetItemTooltip("The stage's OWN collision volumes -- floor planes, out-of-bounds walls, breakable obstacles --\nwalked live from the CStageCollNode scene tree (vtable 0x4E7530). World-space, like the hitboxes.")
	-- Namek destruction meter (Namek = stage 5 only): break scenery to raise g_NamekDestructionPoints@0x500C78;
	-- at >=10 the stage flips to the destroyed layout NEXT round. Live readout doubles as the object-point mapper:
	-- break one object, watch the number jump by its value (small=1 / medium=3 / large=5). RE: sdbz-meter-combo.
	if (memory.read_u32(0x502158) & 0xFF) == 5 then
		local pts = memory.read_u32(0x500C78)
		imgui.Text(string.format("Namek destruction: %d / 10", pts))
		imgui.SameLine(); imgui.TextDisabled("break scenery to raise it; >=10 -> destroyed next round")
		imgui.SetItemTooltip("This is the hidden meter that decides destroyed Namek (it's NOT random). Break one object and\nwatch the jump = that object's point value (small 1 / medium 3 / large 5). Resets each round.")
	end
	row("  floor / ground", "stage_floor", "col_stage_floor")
	row("  walls / out-of-bounds", "stage_walls", "col_stage_wall")
	row("  breakables / obstacles", "stage_obst", "col_stage_obst")
	if cfg.stage then -- live per-element list: uncheck a named collision node to hide it (persisted in cfg.stage_hidden)
		local agg, order, glist = M.stage_elements()
		local tm, tf, te = 0, 0, 0
		for _, nm in ipairs(order) do tm = tm + agg[nm].meshes; tf = tf + agg[nm].faces; te = te + agg[nm].edges end
		imgui.TextDisabled(string.format("    load: %d nodes / %d meshes / %d faces / %d lines drawn", #order, tm, tf, te))
		imgui.TextDisabled(string.format("    collision grids: %d distinct  (breakable = the index-1 grid)", #glist))
		for i = 1, (#glist < 12 and #glist or 12) do
			local gd = glist[i]
			imgui.TextDisabled(string.format("      grid %08X  x%-3d nodes  g12=%-4d  e.g. %s", gd.grid, gd.count, gd.g12, gd.sample))
		end
		if #order == 0 then
			imgui.TextDisabled("    (no collision nodes -- is a battle stage loaded?)")
		elseif imgui.BeginTable then -- own scrollable table (host has the table bindings)
			local h = ((#order < 9 and #order or 9) * 19) + 28
			if imgui.BeginTable("##stagecoll", 3, h) then
				imgui.TableSetupColumn("show", 0.34)
				imgui.TableSetupColumn("element", 1.0)
				imgui.TableSetupColumn("faces", 0.44)
				if imgui.TableSetupScrollFreeze then imgui.TableSetupScrollFreeze(0, 1) end
				imgui.TableHeadersRow()
				for _, nm in ipairs(order) do
					imgui.TableNextRow()
					imgui.TableNextColumn()
					local ch, v = imgui.Checkbox("##sg_" .. nm, not hidden(nm)); if ch then M.set_hidden(nm, not v) end
					imgui.TableNextColumn(); imgui.Text(nm)
					imgui.TableNextColumn(); imgui.Text(tostring(agg[nm].faces))
				end
				imgui.EndTable()
			end
		else -- fallback (host without table bindings yet): plain inline checkboxes
			imgui.TextDisabled(string.format("    elements (%d) -- uncheck to hide:", #order))
			for _, nm in ipairs(order) do
				local ch, v = imgui.Checkbox(string.format("    %s  (%d faces)", nm, agg[nm].faces), not hidden(nm))
				if ch then M.set_hidden(nm, not v) end
			end
		end
	end
	ui.checkbox("State HUD (invuln / guard)", "state")
	ui.checkbox("Hide game HUD", "hide_hud")
	imgui.SetItemTooltip("Hides the whole battle HUD (HP/ki/AC bars, combo counter, round timer, name cards)\nby early-returning HUD_DrawPass@0x309280. Works any time (not just frozen).")
end

return M
