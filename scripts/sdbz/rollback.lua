-- Rollback Phase-0 determinism harness pane (drives the host's rollback.* bindings ->
-- Sdbz/SdbzDeterminism.cpp). Flow: get IN MATCH -> Baseline (snapshots game memory + starts
-- recording pads/checksums) -> play up to the frame cap -> Replay (restores the snapshot, patches
-- the pad fetch off, replays your inputs, compares per-frame chunk checksums). Divergences land in
-- the console + <DataRoot>/sdbz_determinism_report.txt at chunk granularity = the exclude-list
-- shopping list. See sdbz/SDBZ_ROLLBACK_PLAN.md Phase 0.
local M = {}

local cfg, pane
local chunk_kb = 64
local max_secs = 10

function M.init(ctx)
	cfg, pane = ctx.cfg, ctx.pane
end

function M.pane()
	if not pane("Rollback harness (determinism)", "p_rollback") then return end

	imgui.Text("Status:"); imgui.SameLine()
	imgui.TextDisabled(rollback.status())

	if imgui.Button("Baseline + record") then
		rollback.chunk_kb(chunk_kb)
		rollback.max_frames(max_secs * 60)
		rollback.baseline()
	end
	imgui.SetItemTooltip("Snapshot game memory NOW and start recording your inputs + per-frame state checksums.\nDo this mid-match, then just play.")
	imgui.SameLine()
	if imgui.Button("Replay + compare") then rollback.replay() end
	imgui.SetItemTooltip("Restore the snapshot and replay the recorded inputs.\nAny checksum divergence = memory missing from the savestate regions (or needing an exclude).")
	imgui.SameLine()
	if imgui.Button("Stop") then rollback.stop() end

	imgui.SetNextItemWidth(96)
	local ch, v = imgui.InputInt("record secs", max_secs, 1)
	if ch then max_secs = math.max(1, math.min(60, v)) end
	imgui.SameLine(); imgui.SetNextItemWidth(96)
	local ch2, v2 = imgui.InputInt("chunk KB", chunk_kb, 16)
	if ch2 then chunk_kb = math.max(4, math.min(1024, v2)) end

	imgui.TextDisabled("Divergence report: console + sdbz_determinism_report.txt\nA clean replay = the savestate region list is complete.")
end

return M
