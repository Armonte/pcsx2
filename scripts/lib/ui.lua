-- thin imgui.* widget helpers bound to a settings table + a save callback (game-agnostic). Each helper
-- reads/writes cfg[key] and calls save() on change, so a script's control window is mostly one-liners.
-- Shared by every game script via:  local ui = require("ui").bind(cfg, save_cfg)
local M = {}

function M.bind(cfg, save)
	local u = {}

	function u.checkbox(label, key)
		local changed, v = imgui.Checkbox(label, cfg[key])
		if changed then cfg[key] = v; save() end
		return changed
	end

	function u.slider_float(label, key, lo, hi, fmt, logarithmic)
		local changed, v = imgui.SliderFloat(label, cfg[key], lo, hi, fmt, logarithmic)
		if changed then cfg[key] = v; save() end
		return changed
	end

	function u.slider_int(label, key, lo, hi, fmt)
		local changed, v = imgui.SliderInt(label, cfg[key], lo, hi, fmt)
		if changed then cfg[key] = v; save() end
		return changed
	end

	-- {r,g,b,a} colour row: typeable hex field (#RRGGBBAA) + swatch/picker
	function u.color_row(label, key)
		local c = cfg[key]
		imgui.SetNextItemWidth(96)
		local changed, r, g, b, a = imgui.ColorEdit4(label, c[1], c[2], c[3], c[4])
		if changed then cfg[key] = { r, g, b, a }; save() end
		return changed
	end

	return u
end

return M
