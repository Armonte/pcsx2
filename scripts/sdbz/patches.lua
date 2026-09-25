-- SDBZ engine code-patch toggles driven by cfg: hide-HUD, disable-cull, disable-interlace-offset.
-- update() is called once per frame from on_frame; it diffs cfg against the last-applied state and patches /
-- unpatches only on change. init(ctx) wires cfg.
local M = {}
local cfg

function M.init(ctx) cfg = ctx.cfg end

local HUD_DRAWPASS = 0x309280 -- early-return -> whole battle HUD hidden
local CULL_FN      = 0x1C44F0 -- Cull_SphereVsFrustum -> always "visible"
-- GS_VSyncWait_PollField@0x104C00 stores the live interlace field g_GsInterlaceField=(GS_CSR>>13)&1 via
-- `sw $v0,0x3154($v1)` @0x104C68. Patch that store to `sw $zero` (0xAC623154 -> 0xAC603154) to PIN the field to 0,
-- killing the per-field half-line DISPLAY offset (the interlace jitter). This is exactly what PCSX2's
-- "No-Interlacing" pnach for SLUS-214.42 does -- the GS-level interlace settings can't cancel the GAME's offset.
local DEINT_FIELD  = 0x104C68
local DEINT_OFF    = 0xAC603154 -- sw $zero, 0x3154($v1)
local patched = { hide_hud = false, no_cull = false, deint_offset = false }

function M.update()
	if cfg.hide_hud ~= patched.hide_hud then
		patched.hide_hud = cfg.hide_hud
		if cfg.hide_hud then
			engine.patch(HUD_DRAWPASS, 0x03E00008); engine.patch(HUD_DRAWPASS + 4, 0x00000000) -- jr $ra ; nop
		else
			engine.unpatch(HUD_DRAWPASS); engine.unpatch(HUD_DRAWPASS + 4)
		end
	end
	local want_cull = cfg.no_cull or cfg.cam_ortho -- ortho's cone-cull is invalid -> force off
	if want_cull ~= patched.no_cull then
		patched.no_cull = want_cull
		if want_cull then
			engine.patch(CULL_FN, 0x03E00008); engine.patch(CULL_FN + 4, 0x24020001) -- jr $ra ; li $v0,1
		else
			engine.unpatch(CULL_FN); engine.unpatch(CULL_FN + 4)
		end
	end
	if cfg.deint_offset ~= patched.deint_offset then
		patched.deint_offset = cfg.deint_offset
		if cfg.deint_offset then engine.patch(DEINT_FIELD, DEINT_OFF) -- sw $v0 -> sw $zero : pin the interlace field to 0
		else engine.unpatch(DEINT_FIELD) end
	end
end

return M
