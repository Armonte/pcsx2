-- SDBZ state-flag HUD (invuln / throw-invuln / guard-point) -- text overlay drawn from on_frame on the GS
-- thread (uses imgui text metrics, so it must NOT run at capture time). init(ctx) wires cfg; player helpers
-- come from sdbz.players.
local P = require("sdbz.players")
local valid = P.valid

local M = {}
local cfg
local C_WHITE, C_BLUE, C_ORANGE -- packed colors, set in init() once col() is available

function M.init(ctx)
	cfg = ctx.cfg
	C_WHITE  = ctx.col({ 1.00, 1.00, 1.00, 1.00 })
	C_BLUE   = ctx.col({ 0.47, 0.78, 1.00, 1.00 })
	C_ORANGE = ctx.col({ 1.00, 0.78, 0.31, 1.00 })
end

local function hud_put(x, y, s, c)
	draw.text(x, y, c, s)
	return x + imgui.CalcTextWidth(s) + 7.0
end

local function draw_state_hud(player, tag, x, y)
	if not cfg.state then return end
	local f = memory.read_u32(player + 1312)
	x = hud_put(x, y, tag, C_WHITE)
	local ht = memory.read_u32(player + 6208)
	if valid(ht) then
		local iv = memory.read_u32(ht + 12)
		if iv >= 0x80000000 then iv = iv - 0x100000000 end
		if iv > 0 then x = hud_put(x, y, string.format("INVULN:%d", iv), C_BLUE) end
	end
	if (f & 0x200) ~= 0 then x = hud_put(x, y, "THROW-INV", C_BLUE) end
	if (f & 0x2000) ~= 0 then x = hud_put(x, y, "GP-LO", C_ORANGE) end
	if (f & 0x4000) ~= 0 then x = hud_put(x, y, "GP-HI", C_ORANGE) end
end

-- Called from on_frame (GS thread). Master gate lives here so the entry callback stays a one-liner.
function M.draw()
	if not cfg.master then return end
	local hudx, hudy = project.display_to_screen(0.02, 0.06)
	local hud_row = imgui.GetFontSize() + 2.0
	for i = 0, 1 do
		local p = P.get_player(i)
		if p ~= 0 and P.player_valid(p) then
			draw_state_hud(p, (i == 0) and "P1" or "P2", hudx, hudy + i * hud_row)
		end
	end
end

return M
