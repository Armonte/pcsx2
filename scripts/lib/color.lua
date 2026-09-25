-- color packing helpers (game-agnostic). Shared by every game script via require("color").
local M = {}

-- {r,g,b,a} in 0..1 -> ImU32 packed as (R | G<<8 | B<<16 | A<<24) (ImGui's default layout)
function M.pack(c)
	local r = math.floor((c[1] or 1) * 255 + 0.5)
	local g = math.floor((c[2] or 1) * 255 + 0.5)
	local b = math.floor((c[3] or 1) * 255 + 0.5)
	local a = math.floor((c[4] or 1) * 255 + 0.5)
	return (r & 0xFF) | ((g & 0xFF) << 8) | ((b & 0xFF) << 16) | ((a & 0xFF) << 24)
end

return M
