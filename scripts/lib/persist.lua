-- table <-> lua-file persistence (game-agnostic). Handles booleans, numbers, and {r,g,b,a} colour tables --
-- enough for a flat settings table. Shared by every game script via require("persist").
local M = {}

-- write tbl to `path` as a `return { ... }` chunk
function M.save(path, tbl)
	local f = io.open(path, "w")
	if not f then return false end
	f:write("return {\n")
	for k, v in pairs(tbl) do
		if type(v) == "boolean" then
			f:write(string.format("  %s = %s,\n", k, tostring(v)))
		elseif type(v) == "number" then
			f:write(string.format("  %s = %g,\n", k, v))
		elseif type(v) == "table" then
			f:write(string.format("  %s = {%g,%g,%g,%g},\n", k, v[1], v[2], v[3], v[4]))
		end
	end
	f:write("}\n")
	f:close()
	return true
end

-- merge persisted keys from `path` into `tbl` in place (missing keys keep their defaults)
function M.load(path, tbl)
	local f = io.open(path, "r")
	if not f then return end
	local s = f:read("*a"); f:close()
	local chunk = load(s)
	if not chunk then return end
	local ok, t = pcall(chunk)
	if ok and type(t) == "table" then
		for k, v in pairs(t) do tbl[k] = v end
	end
end

return M
