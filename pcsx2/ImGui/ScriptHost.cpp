// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "ImGui/ScriptHost.h"
#include "ImGui/ScriptBridge.h"
#include "Sdbz/SdbzDeterminism.h" // rollback.* Lua table (Phase-0 determinism harness)
#include "Sdbz/SnapshotBench.h" // snap.* Lua table (incremental page-snapshot ring)
#include "Sdbz/RollbackDevice.h"
#include "Sdbz/RbProfiler.h"
#include "Sdbz/PadFeed.h" // rbdev.* Lua table (in-engine rollback device)

#include "Config.h" // EmuFolders
#include "VMManager.h" // disc serial -> per-game script
#include "Host.h"
#include "common/Error.h"
#include "common/StringUtil.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"

#include "imgui.h" // Lua-spawned control windows draw through the imgui.* bindings below

#include <sol/sol.hpp>

#include <atomic>
#include <ctime>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

// Red error banner colour in ImGui's default packed layout (R<<0 | G<<8 | B<<16 | A<<24).
static constexpr uint32_t SDBZ_LUA_ERR_COL = 0xFF3C3CFFu;
// Per-frame instruction budget: a runaway loop in a script becomes a caught error, not a hung GS thread.
static constexpr int SDBZ_LUA_INSN_BUDGET = 20'000'000;

namespace
{
	std::unique_ptr<sol::state> s_state;
	// s_enabled is toggled from the CPU thread (hotkeys). The sol::state is (re)created/destroyed ONLY on the GS
	// thread (RunFrame, via s_loadReq) -- building it off-thread races. It is then RUN from two threads: on_capture
	// on the EE/CPU thread (RunCapture, frame-perfect geometry reads) and on_frame/on_gui on the GS thread
	// (RunFrame/RunGui). One lua_State is not reentrant, so all three hold s_stateMtx.
	std::atomic<bool> s_enabled{false};
	std::atomic<bool> s_loadReq{false};
	bool s_haveOnFrame = false;
	bool s_haveOnGui = false;
	bool s_haveOnCapture = false; // on_capture(): geometry pass, run on the EE/CPU thread
	std::atomic<bool> s_popout{false}; // script wants its on_gui() window in a separate OS window (engine renders it)
	std::mutex s_stateMtx; // serialises the lua_State across the EE thread (RunCapture) and GS thread (RunFrame/RunGui)
	std::string s_error;
	std::string s_path;
	std::time_t s_mtime = 0;
	int s_pollCounter = 0;

	// Hotkey dispatch queue: producers (any thread) push a name; the GS thread drains it before on_frame.
	std::mutex s_dispatchMutex;
	std::vector<std::string> s_dispatchQueue;

	// Per-game script: scripts/<SERIAL>.lua, else the SERIAL=file.lua entry in scripts/games.txt. Searched next
	// to the data root, the app root (exe dir), then the resources dir. Games with no entry run no script: a
	// game script patches that game's memory and must never run on anything else.
	std::string ScriptForSerial(const std::string& root, const std::string& serial)
	{
		const std::string dir = Path::Combine(root, "scripts");
		std::string p = Path::Combine(dir, serial + ".lua");
		if (FileSystem::FileExists(p.c_str()))
			return p;

		const std::optional<std::string> map = FileSystem::ReadFileToString(Path::Combine(dir, "games.txt").c_str());
		if (!map.has_value())
			return {};
		for (std::string_view line : StringUtil::SplitString(map.value(), '\n'))
		{
			line = StringUtil::StripWhitespace(line);
			if (line.empty() || line.front() == '#')
				continue;
			const size_t eq = line.find('=');
			if (eq == std::string_view::npos || StringUtil::StripWhitespace(line.substr(0, eq)) != serial)
				continue;
			p = Path::Combine(dir, StringUtil::StripWhitespace(line.substr(eq + 1)));
			return FileSystem::FileExists(p.c_str()) ? p : std::string();
		}
		return {};
	}

	std::string ResolveScriptPath()
	{
		const std::string serial = VMManager::GetDiscSerial();
		if (serial.empty())
			return {};
		for (const std::string* root : {&EmuFolders::DataRoot, &EmuFolders::AppRoot, &EmuFolders::Resources})
		{
			if (root->empty())
				continue;
			std::string p = ScriptForSerial(*root, serial);
			if (!p.empty())
				return p;
		}
		return {};
	}

	std::time_t FileMtime(const std::string& p)
	{
		FILESYSTEM_STAT_DATA sd;
		return FileSystem::StatFile(p.c_str(), &sd) ? sd.ModificationTime : 0;
	}

	// Hook fired once the per-call instruction budget is exhausted -> abort the script via a Lua error.
	void InsnBudgetHook(lua_State* L, lua_Debug*)
	{
		luaL_error(L, "SDBZ Lua: instruction budget exceeded (infinite loop?)");
	}

	void RegisterApi(sol::state& lua)
	{
		// package is REQUIRED for require() + a writable package.path (the lib/ modules load through it). Without it
		// the package.path assignment below indexes a nil `package` -> unprotected Lua error -> lua panic -> abort().
		lua.open_libraries(sol::lib::base, sol::lib::package, sol::lib::math, sol::lib::table, sol::lib::string,
			sol::lib::os, sol::lib::io);

		auto mem = lua.create_named_table("memory");
		mem.set_function("read_u32", [](uint32_t a) { return ScriptBridge::Read32(a); });
		mem.set_function("read_u16", [](uint32_t a) { return static_cast<uint32_t>(ScriptBridge::Read16(a)); });
		mem.set_function("read_u8", [](uint32_t a) { return static_cast<uint32_t>(ScriptBridge::Read8(a)); });
		mem.set_function("read_f32", [](uint32_t a) { return ScriptBridge::ReadF32(a); });
		mem.set_function("write_u32", [](uint32_t a, uint32_t v) { ScriptBridge::WriteData32(a, v); });
		mem.set_function("write_f32", [](uint32_t a, float v) { ScriptBridge::WriteDataF32(a, v); });
		mem.set_function("write_u8", [](uint32_t a, uint32_t v) { ScriptBridge::WriteData8(a, static_cast<uint8_t>(v)); });
		mem.set_function("valid", [](uint32_t a) { return a >= 0x100000u && a < 0x2000000u; });
		mem.set_function("ready", []() { return ScriptBridge::MemReady(); });

		// rollback Phase-0 determinism harness (Sdbz/SdbzDeterminism): baseline -> play inputs ->
		// replay -> divergence report at chunk granularity. Region/exclude knowledge stays script-side.
		auto rb = lua.create_named_table("rollback");
		rb.set_function("baseline", []() { SdbzDeterminism::CaptureBaseline(); });
		rb.set_function("replay", []() { SdbzDeterminism::StartReplay(); });
		rb.set_function("stop", []() { SdbzDeterminism::Stop(); });
		rb.set_function("status", []() { return SdbzDeterminism::Status(); });
		rb.set_function("set_region", [](uint32_t a, uint32_t b) { SdbzDeterminism::SetRegion(a, b); });
		rb.set_function("add_exclude", [](uint32_t a, uint32_t n) { SdbzDeterminism::AddExclude(a, n); });
		rb.set_function("clear_excludes", []() { SdbzDeterminism::ClearExcludes(); });
		rb.set_function("chunk_kb", [](uint32_t kb) { SdbzDeterminism::SetChunkKB(kb); });
		rb.set_function("max_frames", [](uint32_t n) { SdbzDeterminism::SetMaxFrames(n); });

		// Incremental rollback snapshots (Sdbz/PageSnapshotRing): capture every frame, only dirty
		// 4 KiB pages are copied. snap.start(capacity, write_protect) after add_region/add_exclude.
		auto sn = lua.create_named_table("snap");
		sn.set_function("clear_regions", []() { SnapshotBench::ClearRegions(); });
		sn.set_function("add_region", [](uint32_t a, uint32_t n) { SnapshotBench::AddRegion(a, n); });
		sn.set_function("add_exclude", [](uint32_t a, uint32_t n) { SnapshotBench::AddExclude(a, n); });
		sn.set_function("start", [](uint32_t cap, bool wp) { SnapshotBench::Start(cap, wp); });
		sn.set_function("stop", []() { SnapshotBench::Stop(); });
		sn.set_function("rollback", [](uint32_t n) { SnapshotBench::Rollback(n); });
		sn.set_function("verify", [](bool on) { SnapshotBench::SetVerify(on); });
		sn.set_function("status", []() { return SnapshotBench::Status(); });

		// In-engine rollback device (Sdbz/RollbackDevice): the game-side hook (installed by the game script) calls
		// it via syscall; these configure it. rbdev.start(mode 0 off|1 capture|2 synctest, rollback_frames, write_protect)
		auto rd = lua.create_named_table("rbdev");
		rd.set_function("clear", []() { RollbackDevice::ClearConfig(); });
		rd.set_function("add_region", [](uint32_t a, uint32_t n) { RollbackDevice::AddRegion(a, n); });
		rd.set_function("add_exclude", [](uint32_t a, uint32_t n) { RollbackDevice::AddExclude(a, n); });
		// set_dynamic_excludes({ {addr, len}, ... }): replaceable while running (ring rebuilt at the next frame)
		rd.set_function("set_dynamic_excludes", [](sol::table t) {
			std::vector<std::pair<u32, u32>> v;
			v.reserve(t.size());
			for (const auto& kv : t)
			{
				sol::table r = kv.second.as<sol::table>();
				v.emplace_back(r.get<uint32_t>(1), r.get<uint32_t>(2));
			}
			RollbackDevice::SetDynamicExcludes(v);
		});
		rd.set_function("set_input_block", [](uint32_t a, uint32_t n) { RollbackDevice::SetInputBlock(a, n); });
		rd.set_function("add_ignore", [](uint32_t a, uint32_t n) { RollbackDevice::AddCompareIgnore(a, n); });
		rd.set_function("add_watch", [](uint32_t a, uint32_t n, const std::string& name) { RollbackDevice::AddWatch(a, n, name); });
		rd.set_function("start", [](int mode, uint32_t frames, bool wp) { RollbackDevice::Start(static_cast<RollbackDevice::Mode>(mode), frames, wp); });
		rd.set_function("stop", []() { RollbackDevice::Stop(); });
		rd.set_function("set_gate", [](uint32_t a) { RollbackDevice::SetGate(a); });
		rd.set_function("set_rng_split", [](uint32_t a, uint32_t n) { RollbackDevice::SetRngSplit(a, n); });
		rd.set_function("add_gate_stable", [](uint32_t a, uint32_t n) { RollbackDevice::AddGateStable(a, n); });
		rd.set_function("set_rng_trace", [](bool on) { RollbackDevice::SetRngTrace(on); });
		rd.set_function("add_trace_alias", [](uint32_t from, uint32_t to) { RollbackDevice::AddTraceAlias(from, to); });
		rd.set_function("status", []() { return RollbackDevice::Status(); });
		rd.set_function("report", []() { return RollbackDevice::ReportText(); });
		// in-process sampling profiler of the EE thread, tagged by rollback phase (Windows)
		rd.set_function("prof_start", [](sol::optional<uint32_t> hz) { RbProfiler::Start(hz.value_or(2000)); });
		rd.set_function("prof_stop", []() { RbProfiler::Stop(); });
		// Deterministic controller feed (player 1-based; buttons = game layout, see PadFeed.h):
		//   pad_off(p) | pad_const(p, buttons[, lx, ly, rx, ry]) | pad_seq(p, {{buttons, frames[, lx, ly, rx, ry]}, ...}[, loop])
		//   pad_mash(p, seed, mask, min_hold, max_hold) | pad_status()
		rd.set_function("pad_off", [](uint32_t p) { PadFeed::Off(p - 1); });
		rd.set_function("pad_const", [](uint32_t p, uint32_t b, sol::optional<int> lx, sol::optional<int> ly,
										 sol::optional<int> rx, sol::optional<int> ry) {
			PadFeed::Step s;
			s.buttons = static_cast<u16>(b);
			s.lx = static_cast<u8>(lx.value_or(0x80)); s.ly = static_cast<u8>(ly.value_or(0x80));
			s.rx = static_cast<u8>(rx.value_or(0x80)); s.ry = static_cast<u8>(ry.value_or(0x80));
			PadFeed::Const(p - 1, s);
		});
		rd.set_function("pad_seq", [](uint32_t p, sol::table t, sol::optional<bool> loop) {
			std::vector<PadFeed::Step> steps;
			for (size_t i = 1; i <= t.size(); i++)
			{
				sol::table e = t[i];
				PadFeed::Step s;
				s.buttons = static_cast<u16>(e.get_or(1, 0u));
				s.frames = e.get_or(2, 1u);
				s.lx = static_cast<u8>(e.get_or(3, 0x80)); s.ly = static_cast<u8>(e.get_or(4, 0x80));
				s.rx = static_cast<u8>(e.get_or(5, 0x80)); s.ry = static_cast<u8>(e.get_or(6, 0x80));
				steps.push_back(s);
			}
			PadFeed::Sequence(p - 1, std::move(steps), loop.value_or(false));
		});
		rd.set_function("pad_mash", [](uint32_t p, uint32_t seed, uint32_t mask, uint32_t lo, uint32_t hi) {
			PadFeed::Mash(p - 1, seed, static_cast<u16>(mask), lo, hi);
		});
		rd.set_function("pad_status", []() { return PadFeed::Status(); });
		rd.set_function("prof_report", [](sol::optional<uint32_t> top) { return RbProfiler::Report(top.value_or(25)); });
		rd.set("MAGIC", RollbackDevice::MAGIC);

		auto proj = lua.create_named_table("project");
		proj.set_function("world_to_screen", [](float x, float y, float z) {
			float sx = 0.0f, sy = 0.0f;
			bool ok = ScriptBridge::WorldToScreen(x, y, z, sx, sy);
			return std::make_tuple(sx, sy, ok);
		});
		// transform a local point by the 4x4 stored at matAddr (e.g. a bone's world matrix) -> world point
		proj.set_function("apply_matrix_at", [](uint32_t matAddr, float x, float y, float z) {
			float W[16];
			ScriptBridge::ReadMatrix(matAddr, W);
			float o[3];
			ScriptBridge::ApplyMatrix(W, x, y, z, o);
			return std::make_tuple(o[0], o[1], o[2]);
		});
		// 0..1 display coords -> window pixels (resolution-independent HUD anchor)
		proj.set_function("display_to_screen", [](float u, float v) {
			float sx = 0.0f, sy = 0.0f;
			bool ok = ScriptBridge::DisplayToScreen(u, v, sx, sy);
			return std::make_tuple(sx, sy, ok);
		});
		// the script tells the engine which world->screen matrix to project with (game-specific address)
		proj.set_function("set_world_matrix", [](uint32_t addr) { ScriptBridge::SetWorldMatrix(addr); });

		auto draw = lua.create_named_table("draw");
		// matAddr = EE address of the bone's 4x4 world matrix (node+240); center/radius are in that frame.
		draw.set_function("sphere", [](uint32_t matAddr, float cx, float cy, float cz, float r, uint32_t col) {
			float W[16];
			ScriptBridge::ReadMatrix(matAddr, W);
			ScriptBridge::DrawSphere(W, cx, cy, cz, r, col);
		});
		// world-space sphere (identity matrix) -- e.g. projectile/shell capsules that already hold world points
		draw.set_function("sphere_world", [](float cx, float cy, float cz, float r, uint32_t col) {
			ScriptBridge::DrawSphereWorld(cx, cy, cz, r, col);
		});
		draw.set_function("line_world",
			[](float x1, float y1, float z1, float x2, float y2, float z2, uint32_t col, float th) {
				ScriptBridge::DrawLineWorld(x1, y1, z1, x2, y2, z2, col, th);
			});
		// Native batched collision-mesh face draw -- ONE call per mesh does the whole transform/project/emit loop in
		// C++ on live EE memory (the per-vertex apply_matrix_at + per-edge line_world in Lua were the FPS sink). The
		// script passes the addresses + record/pool layout; floorRgba/wallRgba of 0 skip that category. See
		// ScriptBridge::DrawMeshFaces. (matAddr, poolAddr, recsAddr, faceCount, recStride, vcountOff, startOff,
		// normalOff, floorRgba, wallRgba, ceilThresh, thick)
		draw.set_function("collision_mesh",
			[](uint32_t matAddr, uint32_t poolAddr, uint32_t recsAddr, int faceCount, int recStride, int vcountOff,
				int startOff, int normalOff, uint32_t floorRgba, uint32_t wallRgba, float ceilThresh, sol::optional<float> th) {
				ScriptBridge::DrawMeshFaces(matAddr, poolAddr, recsAddr, faceCount, recStride, vcountOff, startOff,
					normalOff, floorRgba, wallRgba, ceilThresh, th.value_or(1.5f));
			});
		// Native walk+draw of a whole collision subtree -- ONE call per frame draws the entire stage (replaces the
		// ~600 per-mesh dispatches). All offsets come from the opts table (game knowledge stays in Lua); the engine
		// DFS-walks live EE memory, dedups shared edges, caches vertex projections. See ScriptBridge::DrawCollisionTree.
		draw.set_function("collision_tree", [](sol::table o) {
			ScriptBridge::CollTreeDraw p{};
			p.root = o.get_or("root", static_cast<uint32_t>(0));
			p.floorRgba = o.get_or("floor", static_cast<uint32_t>(0));
			p.wallRgba = o.get_or("wall", static_cast<uint32_t>(0));
			p.obstRgba = o.get_or("obst", static_cast<uint32_t>(0));
			p.ceilThresh = o.get_or("ceil", 0.6f);
			p.thick = o.get_or("thick", 1.5f);
			p.childOff = o.get_or("child_off", 16);
			p.sibOff = o.get_or("sib_off", 8);
			p.matOff = o.get_or("mat_off", 240);
			p.nameOff = o.get_or("name_off", 20);
			p.gridOff = o.get_or("grid_off", 436);
			p.brkGrid = o.get_or("brk_grid", static_cast<uint32_t>(0));
			p.headOff = o.get_or("head_off", 440);
			p.ownerOff = o.get_or("owner_off", 180);
			p.nextOff = o.get_or("next_off", 176);
			p.countOff = o.get_or("count_off", 12);
			p.poolOff = o.get_or("pool_off", 16);
			p.recsOff = o.get_or("recs_off", 20);
			p.centerOff = o.get_or("center_off", 208);
			p.radiusOff = o.get_or("radius_off", 204);
			p.recStride = o.get_or("rec_stride", 32);
			p.vcountOff = o.get_or("vcount_off", 0);
			p.startOff = o.get_or("start_off", 4);
			p.normalOff = o.get_or("normal_off", 20);
			const std::string hid = o.get_or("hidden", std::string());
			p.hidden = hid.c_str();
			ScriptBridge::DrawCollisionTree(p);
		});
		draw.set_function("text", [](float sx, float sy, uint32_t col, const char* s) {
			ScriptBridge::DrawTextScreen(sx, sy, col, s ? s : "");
		});
		// screen-space 2D primitives (for HUD widgets like the view-axis gizmo); coords in overlay pixels
		draw.set_function("line2d", [](float x0, float y0, float x1, float y1, uint32_t col, sol::optional<float> th) {
			ScriptBridge::DrawLineScreen(x0, y0, x1, y1, col, th.value_or(1.5f));
		});
		draw.set_function("circle2d", [](float cx, float cy, float r, uint32_t col, sol::optional<bool> filled, sol::optional<float> th) {
			ScriptBridge::DrawCircleScreen(cx, cy, r, col, filled.value_or(false), th.value_or(1.5f));
		});
		// 2D filled triangle (nav-cube faces etc). depth = painter sort key; default 1.0 keeps HUD fills on top of the
		// (large clip-w) 3D scene fills. Vary depth per face for back-to-front layering within a widget.
		draw.set_function("tri2d", [](float x0, float y0, float x1, float y1, float x2, float y2, uint32_t col, sol::optional<float> depth) {
			ScriptBridge::DrawTriScreen(x0, y0, x1, y1, x2, y2, col, depth.value_or(1.0f));
		});
		draw.set_function("set_sphere_style", [](int segments, bool shaded, float fill_alpha, float thickness) {
			ScriptBridge::SetSphereStyle(segments, shaded, fill_alpha, thickness);
		});

		// engine = generic, game-agnostic services only (no SDBZ knowledge -- the script owns players / freeze /
		// freecam / training via memory + patch + input below).
		auto eng = lua.create_named_table("engine");
		eng.set_function("set_cursor", [](bool on) { ScriptBridge::SetCursorVisible(on); });
		eng.set_function("set_gamepad_nav", [](bool on) { ScriptBridge::SetGamepadNav(on); });
		eng.set_function("set_popout", [](bool on) { Script::SetPopout(on); }); // pop the script's GUI into its own window
		eng.set_function("claim_mouse", [](bool on) { ScriptBridge::SetMouseClaimed(on); }); // over a script widget (gizmo) -> no dbl-click fullscreen
		eng.set_function("set_frame_delay", [](int n) { ScriptBridge::SetFrameDelay(n); });
		// recompiler-safe EE code patching, for script-owned freeze/freecam NOPs (saves+restores the original)
		eng.set_function("patch", [](uint32_t addr, uint32_t word) { ScriptBridge::PatchCode(addr, word); });
		eng.set_function("unpatch", [](uint32_t addr) { ScriptBridge::UnpatchCode(addr); });
		// savestates for fast iteration (queued on the CPU thread; states are saved without script patches and a
		// load reconciles them, then on_state_load(reapplied, kept, dropped) runs on the next frame)
		eng.set_function("save_state", [](std::string path) {
			Host::RunOnCPUThread([path]() {
				VMManager::SaveState(path.c_str(), false, false, [path](const std::string& err) {
					Console.ErrorFmt("[Script] save_state {} failed: {}", path, err);
				});
				Console.WriteLnFmt("[Script] state saved: {}", path);
			}, false);
		});
		eng.set_function("load_state", [](std::string path) {
			Host::RunOnCPUThread([path]() {
				Error err;
				if (VMManager::LoadState(path.c_str(), &err))
					Console.WriteLnFmt("[Script] state loaded: {}", path);
				else
					Console.ErrorFmt("[Script] load_state {} failed: {}", path, err.GetDescription());
			}, false);
		});
		eng.set_function("set_aspect", [](int i) { ScriptBridge::SetAspect(i); });
		eng.set_function("get_aspect", []() { return ScriptBridge::GetAspect(); });
		eng.set_function("set_deinterlace", [](int m) { ScriptBridge::SetDeinterlace(m); });
		eng.set_function("get_deinterlace", []() { return ScriptBridge::GetDeinterlace(); });
		eng.set_function("set_interlace_offset", [](bool b) { ScriptBridge::SetInterlaceOffset(b); });
		eng.set_function("get_interlace_offset", []() { return ScriptBridge::GetInterlaceOffset(); });

		// input (script-driven freecam etc.) -- Windows VK_* keys + ImGui mouse
		auto inp = lua.create_named_table("input");
		inp.set_function("key_down", [](int vk) { return ScriptBridge::KeyDown(vk); });
		inp.set_function("mouse_delta", []() { float dx = 0, dy = 0; ScriptBridge::MouseDelta(dx, dy); return std::make_tuple(dx, dy); });
		inp.set_function("mouse_pos", []() { float x = 0, y = 0; ScriptBridge::MousePos(x, y); return std::make_tuple(x, y); });
		inp.set_function("mouse_down", [](int b) { return ScriptBridge::MouseDown(b); });
		inp.set_function("wheel", []() { return ScriptBridge::MouseWheel(); });
		// raw RMB look delta (recentered, edge-free) -- the 1:1 freecam feel; (0,0) when RMB up
		inp.set_function("look_delta", []() { float dx = 0, dy = 0; ScriptBridge::LookDelta(dx, dy); return std::make_tuple(dx, dy); });
		inp.set_function("want_mouse", []() { return ScriptBridge::WantMouse(); });

		// ImGui (immediate-mode) so scripts spawn their OWN windows/controls -- no native per-game UI needed.
		// Called inside RenderOverlays (a live ImGui frame), so Begin/End/widgets work directly. Value-editing
		// widgets return (changed, new_value...) -- the script keeps the value in its own settings table.
		auto ig = lua.create_named_table("imgui");
		// Begin(name [, gamepad_nav]). DEFAULT: NoNav -- the window is excluded from ImGui keyboard/gamepad
		// navigation, so a controller keeps driving the GAME instead of moving the overlay's cursor. Pass true to
		// opt this window into nav. (Per-window only; PCSX2's own nav / big-picture UI is untouched.)
		ig.set_function("Begin", [](const char* name, sol::optional<bool> nav, sol::optional<bool> fill) {
			// NoFocusOnAppearing: the window never steals focus from the game just by showing up.
			ImGuiWindowFlags flags = ImGuiWindowFlags_NoFocusOnAppearing;
			if (!nav.value_or(false)) flags |= ImGuiWindowFlags_NoNav;
			// fill = the host is filling a window for us (the popout): drop the title bar / chrome and lock it.
			if (fill.value_or(false))
				flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
					ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus;
			return ImGui::Begin(name ? name : "##script", nullptr, flags);
		});
		// window geometry -- lets the script persist its control window's position/size in its own cfg
		// (gpbear: "save lua window size/position"). Get* = current window (call between Begin/End);
		// SetNext* apply to the NEXT Begin; once=true -> ImGuiCond_Once (applies a single time per session,
		// so the user can still drag/resize afterwards).
		ig.set_function("GetWindowPos", []() {
			const ImVec2 p = ImGui::GetWindowPos();
			return std::make_tuple(p.x, p.y);
		});
		ig.set_function("GetWindowSize", []() {
			const ImVec2 s = ImGui::GetWindowSize();
			return std::make_tuple(s.x, s.y);
		});
		ig.set_function("SetNextWindowPos", [](float x, float y, sol::optional<bool> once) {
			ImGui::SetNextWindowPos(ImVec2(x, y), once.value_or(false) ? ImGuiCond_Once : ImGuiCond_Always);
		});
		ig.set_function("SetNextWindowSize", [](float x, float y, sol::optional<bool> once) {
			ImGui::SetNextWindowSize(ImVec2(x, y), once.value_or(false) ? ImGuiCond_Once : ImGuiCond_Always);
		});
		ig.set_function("End", []() { ImGui::End(); });
		ig.set_function("Text", [](const char* s) { ImGui::TextUnformatted(s ? s : ""); });
		ig.set_function("TextDisabled", [](const char* s) { ImGui::TextDisabled("%s", s ? s : ""); });
		ig.set_function("Separator", []() { ImGui::Separator(); });
		// SameLine(offset): 0 = default item spacing; >0 = absolute x from line start (native used 168/110)
		ig.set_function("SameLine", [](sol::optional<float> off) { ImGui::SameLine(off.value_or(0.0f)); });
		ig.set_function("Spacing", []() { ImGui::Spacing(); });
		ig.set_function("Indent", [](sol::optional<float> w) { ImGui::Indent(w.value_or(0.0f)); });
		ig.set_function("Unindent", [](sol::optional<float> w) { ImGui::Unindent(w.value_or(0.0f)); });
		ig.set_function("PushID", [](int id) { ImGui::PushID(id); });
		ig.set_function("PopID", []() { ImGui::PopID(); });
		ig.set_function("SetNextItemWidth", [](float w) { ImGui::SetNextItemWidth(w); });
		ig.set_function("BeginDisabled", [](sol::optional<bool> d) { ImGui::BeginDisabled(d.value_or(true)); });
		ig.set_function("EndDisabled", []() { ImGui::EndDisabled(); });
		ig.set_function("SetItemTooltip", [](const char* s) { ImGui::SetItemTooltip("%s", s ? s : ""); });
		// Scrollable child region + tables -- so a script can put a long list (e.g. the stage-collision element list)
		// in its OWN fixed-height scroll box instead of growing the whole window.
		ig.set_function("BeginChild",
			[](const char* id, sol::optional<float> w, sol::optional<float> h, sol::optional<bool> border) {
				return ImGui::BeginChild(id ? id : "##child", ImVec2(w.value_or(0.0f), h.value_or(0.0f)),
					border.value_or(false) ? ImGuiChildFlags_Borders : ImGuiChildFlags(0));
			});
		ig.set_function("EndChild", []() { ImGui::EndChild(); });
		// BeginTable(id, columns [, height]): ScrollY + RowBg + borders + proportional sizing baked in; height>0 caps
		// it to a scroll box of that pixel height. Pair with TableSetupColumn(label[,weight]) / TableHeadersRow /
		// TableNextRow / TableNextColumn. Only call EndTable() when BeginTable() returned true.
		ig.set_function("BeginTable", [](const char* id, int cols, sol::optional<float> h) {
			const ImGuiTableFlags f = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
				ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp;
			return ImGui::BeginTable(id ? id : "##tbl", cols < 1 ? 1 : cols, f, ImVec2(0.0f, h.value_or(0.0f)));
		});
		ig.set_function("EndTable", []() { ImGui::EndTable(); });
		ig.set_function("TableNextRow", []() { ImGui::TableNextRow(); });
		ig.set_function("TableNextColumn", []() { return ImGui::TableNextColumn(); });
		ig.set_function("TableSetupColumn", [](const char* label, sol::optional<float> weight) {
			ImGui::TableSetupColumn(label ? label : "", ImGuiTableColumnFlags_WidthStretch, weight.value_or(1.0f));
		});
		ig.set_function("TableSetupScrollFreeze", [](int c, int r) { ImGui::TableSetupScrollFreeze(c, r); });
		ig.set_function("TableHeadersRow", []() { ImGui::TableHeadersRow(); });
		ig.set_function("Button", [](const char* label) { return ImGui::Button(label ? label : "##b"); });
		// Collapsible header with PERSISTED open-state: seed from `open` once, return live state (1:1 native pane()).
		ig.set_function("CollapsingHeader", [](const char* label, bool open) {
			ImGui::SetNextItemOpen(open, ImGuiCond_Once);
			return ImGui::CollapsingHeader(label ? label : "##h");
		});
		ig.set_function("Checkbox", [](const char* label, bool v) {
			bool changed = ImGui::Checkbox(label ? label : "##c", &v);
			return std::make_tuple(changed, v);
		});
		ig.set_function("SliderInt", [](const char* label, int v, int lo, int hi, sol::optional<std::string> fmt) {
			bool changed = ImGui::SliderInt(label ? label : "##si", &v, lo, hi, fmt ? fmt->c_str() : "%d");
			return std::make_tuple(changed, v);
		});
		ig.set_function("SliderFloat",
			[](const char* label, float v, float lo, float hi, sol::optional<std::string> fmt, sol::optional<bool> logarithmic) {
				const ImGuiSliderFlags fl = (logarithmic && *logarithmic) ? ImGuiSliderFlags_Logarithmic : 0;
				bool changed = ImGui::SliderFloat(label ? label : "##sf", &v, lo, hi, fmt ? fmt->c_str() : "%.3f", fl);
				return std::make_tuple(changed, v);
			});
		ig.set_function("SliderFloat3",
			[](const char* label, float x, float y, float z, float lo, float hi, sol::optional<std::string> fmt) {
				float v[3] = {x, y, z};
				bool changed = ImGui::SliderFloat3(label ? label : "##sf3", v, lo, hi, fmt ? fmt->c_str() : "%.3f");
				return std::make_tuple(changed, v[0], v[1], v[2]);
			});
		// Typeable numeric fields: click and type an EXACT value (no ctrl+click gesture needed), with optional
		// +/- step buttons. InputFloat shows the real float to its format (default %.4f) -- no rounding to int.
		// Both return (changed, value); changed fires on commit (Enter / focus-loss) or a step-button press.
		ig.set_function("InputInt", [](const char* label, int v, sol::optional<int> step) {
			const int s = step.value_or(0);
			bool changed = ImGui::InputInt(label ? label : "##ii", &v, s, s * 10);
			return std::make_tuple(changed, v);
		});
		ig.set_function("InputFloat",
			[](const char* label, float v, sol::optional<float> step, sol::optional<std::string> fmt) {
				const float s = step.value_or(0.0f);
				bool changed = ImGui::InputFloat(label ? label : "##if", &v, s, s * 10.0f, fmt ? fmt->c_str() : "%.4f");
				return std::make_tuple(changed, v);
			});
		// swatch + a typeable hex field (#RRGGBBAA): click the hex and type to enter an exact colour; click the
		// swatch for the picker (AlphaBar keeps the alpha bar there). Previously NoInputs hid the field, so the
		// colour rows couldn't be edited by hand.
		ig.set_function("ColorEdit4", [](const char* label, float r, float g, float b, float a) {
			float c[4] = {r, g, b, a};
			bool changed = ImGui::ColorEdit4(label ? label : "##ce", c,
				ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_DisplayHex);
			return std::make_tuple(changed, c[0], c[1], c[2], c[3]);
		});
		// Combo from a Lua array of strings (0-based current index)
		ig.set_function("Combo", [](const char* label, int current, sol::table items) {
			std::vector<std::string> strs;
			strs.reserve(items.size());
			for (std::size_t i = 1; i <= items.size(); i++)
				strs.push_back(items.get<std::string>(i));
			std::vector<const char*> ptrs;
			ptrs.reserve(strs.size());
			for (const std::string& s : strs)
				ptrs.push_back(s.c_str());
			bool changed = ImGui::Combo(label ? label : "##combo", &current, ptrs.data(), static_cast<int>(ptrs.size()));
			return std::make_tuple(changed, current);
		});
		ig.set_function("TreeNode", [](const char* label) { return ImGui::TreeNode(label ? label : "##t"); });
		ig.set_function("TreePop", []() { ImGui::TreePop(); });
		ig.set_function("CalcTextWidth", [](const char* s) { return ImGui::CalcTextSize(s ? s : "").x; });
		ig.set_function("GetFontSize", []() { return ImGui::GetFontSize(); });
		// keyboard diagnostic for the CURRENT imgui context (in-game or popout): (KeyCtrl, WantTextInput,
		// WantCaptureKeyboard). Lets a script confirm whether Ctrl + the active-item text capture are reaching ImGui
		// when debugging Ctrl+click slider entry.
		ig.set_function("IoFlags", []() {
			const ImGuiIO& io = ImGui::GetIO();
			return std::make_tuple(io.KeyCtrl, io.WantTextInput, io.WantCaptureKeyboard);
		});
	}

	// Build a fresh state and run the script. On success, atomically swap it in; on failure keep the
	// last-good state (if any) running and surface the error.
	bool LoadScript()
	{
		const std::string path = ResolveScriptPath();
		if (path != s_path && s_state)
		{
			// Different game (or none): the old script's patches and state belong to the old game.
			ScriptBridge::UnpatchAll();
			s_state.reset();
			s_haveOnFrame = s_haveOnGui = s_haveOnCapture = false;
		}
		s_path = path;
		s_error.clear();
		if (s_path.empty())
			return false; // no script for this game

		// PCSX2 builds with exceptions off; sol2 detects this and protects via Lua pcall instead. With
		// sol::script_pass_on_error a syntax/runtime error in the script is RETURNED here, not thrown.
		auto st = std::make_unique<sol::state>();
		RegisterApi(*st);
		const std::string dir(Path::GetDirectory(s_path)); // GetDirectory returns string_view -> construct explicitly
		(*st)["SCRIPT_PATH"] = s_path;
		(*st)["GAME_SERIAL"] = VMManager::GetDiscSerial(); // one script can serve several versions (NA/JP/PAL/arcade)                       // so scripts can persist their own settings
		(*st)["SCRIPT_DIR"] = dir;                           // (io is open) -- e.g. SCRIPT_DIR.."/sdbz.cfg"
		// shared Lua "library": require() searches the script dir + a lib/ subdir, so sdbz.lua / fuc.lua reuse modules
		(*st)["package"]["path"] = dir + "/?.lua;" + dir + "/lib/?.lua";
		sol::protected_function_result r = st->safe_script_file(s_path, sol::script_pass_on_error);
		if (!r.valid())
		{
			sol::error e = r;
			s_error = e.what();
			Console.ErrorFmt("[Script] load error: {}", s_error);
			return false; // keep last-good
		}

		// Revert the OUTGOING script's code patches before swapping in the new state. The patch registry is a
		// static that outlives the sol::state, so without this a freeze/freecam NOP (notably the round-timer NOP)
		// would survive the reload and freeze the timer with freeze showing OFF. The new state re-asserts its
		// cfg-driven patches on its first on_frame. (No-op on the very first load -- nothing patched yet.)
		ScriptBridge::UnpatchAll();

		s_state = std::move(st);
		s_haveOnFrame = ((*s_state)["on_frame"].get_type() == sol::type::function);
		s_haveOnGui = ((*s_state)["on_gui"].get_type() == sol::type::function);
		s_haveOnCapture = ((*s_state)["on_capture"].get_type() == sol::type::function);
		s_mtime = FileMtime(s_path);
		s_error.clear();
		Console.WriteLnFmt("[Script] loaded {} (on_frame={})", s_path, s_haveOnFrame);
		return true;
	}

	// Throttled mtime poll so the script hot-reloads on save without a rebuild.
	void PollReload()
	{
		if (++s_pollCounter < 30)
			return;
		s_pollCounter = 0;
		if (ResolveScriptPath() != s_path)
		{
			Console.WriteLn("[Script] game changed -> switching script");
			LoadScript();
			return;
		}
		if (s_path.empty())
			return;
		const std::time_t m = FileMtime(s_path);
		if (m != 0 && m != s_mtime)
		{
			Console.WriteLn("[Script] script changed on disk -> reloading");
			LoadScript();
		}
	}

	// GS thread: deliver any queued hotkey names to the script's on_hotkey(name). The instruction-budget hook
	// must already be armed (RunFrame does it). A missing on_hotkey just drops the names.
	void DrainDispatch()
	{
		std::vector<std::string> pending;
		{
			std::lock_guard<std::mutex> lk(s_dispatchMutex);
			if (s_dispatchQueue.empty())
				return;
			pending.swap(s_dispatchQueue);
		}
		if (!s_state)
			return;
		sol::protected_function fn = (*s_state)["on_hotkey"];
		if (fn.get_type() != sol::type::function)
			return;
		for (const std::string& name : pending)
		{
			sol::protected_function_result r = fn(name);
			if (!r.valid())
			{
				sol::error e = r;
				s_error = e.what();
				Console.ErrorFmt("[Script] on_hotkey error: {}", s_error);
			}
		}
	}
	// Run a script global (on_frame / on_gui) protected, in the CURRENT imgui context. Caller arms the hook and
	// guarantees s_state. show_banner draws the error in-game (only valid in the main context with a draw list).
	void RunProtected(const char* name, bool show_banner)
	{
		sol::protected_function fn = (*s_state)[name];
		sol::protected_function_result r = fn();
		if (!r.valid())
		{
			sol::error e = r;
			s_error = e.what();
			Console.ErrorFmt("[Script] {} error: {}", name, s_error);
			if (show_banner)
				ScriptBridge::DrawTextScreen(24.0f, 24.0f, SDBZ_LUA_ERR_COL, ("Lua error: " + s_error).c_str());
		}
		else if (!s_error.empty())
		{
			s_error.clear(); // recovered (e.g. after a hot-reload fix)
		}
	}
} // namespace

namespace Script
{
	bool IsEnabled()
	{
		return s_enabled.load();
	}

	bool IsActive()
	{
		return s_enabled.load() && s_state && s_haveOnFrame;
	}

	void SetEnabled(bool on)
	{
		// Hotkey -> CPU thread. NEVER touch the sol::state here -- just flip the flag and (on enable) request a
		// load that RunFrame performs on the GS thread. Building the state off-thread races RunFrame -> crash.
		const bool was = s_enabled.exchange(on);
		if (on && !was)
			s_loadReq.store(true); // first enable this session -> GS thread loads it
		else if (!on && was)
			ScriptBridge::UnpatchAll(); // disabling the overlay must revert its code patches (else a stale freeze
			                            // NOP keeps the timer frozen); RunOnCPUThread is safe from this thread.
		Console.WriteLn(on ? "[Script] enabled" : "[Script] disabled");
	}

	void Reload()
	{
		// Deferred to the GS thread (same reason as SetEnabled) -- request it; RunFrame does the actual reload.
		s_loadReq.store(true);
		Console.WriteLn("[Script] reload requested");
	}

	void RunFrame()
	{
		if (!s_enabled.load())
			return;
		std::lock_guard<std::mutex> lk(s_stateMtx); // serialise vs RunCapture (EE thread) / RunGui

		// The sol::state is (re)created here, on the GS thread, before it's run. Consumed once per enable/reload
		// request (a failed load shows the banner below; the user re-requests via the Reload hotkey).
		if (s_loadReq.exchange(false))
			LoadScript();

		PollReload();

		if (!s_state || (!s_haveOnFrame && !s_haveOnGui))
		{
			if (!s_error.empty())
				ScriptBridge::DrawTextScreen(24.0f, 24.0f, SDBZ_LUA_ERR_COL, ("Lua: " + s_error).c_str());
			return;
		}

		// Re-arm the per-call instruction budget each frame (resets the count).
		lua_sethook(s_state->lua_state(), InsnBudgetHook, LUA_MASKCOUNT, SDBZ_LUA_INSN_BUDGET);
		DrainDispatch(); // deliver queued hotkeys (on_hotkey) before this frame

		// A savestate was loaded since the last frame: tell the script (it re-arms its own state; code patches were
		// already reconciled on the CPU thread) -- on_state_load(reapplied, kept, dropped).
		static u32 s_seen_state_load = ScriptBridge::StateLoadSerial();
		if (const u32 serial = ScriptBridge::StateLoadSerial(); serial != s_seen_state_load)
		{
			s_seen_state_load = serial;
			sol::protected_function fn = (*s_state)["on_state_load"];
			if (fn.valid())
			{
				u32 reapplied, kept, dropped;
				ScriptBridge::StateLoadStats(reapplied, kept, dropped);
				sol::protected_function_result r = fn(reapplied, kept, dropped);
				if (!r.valid())
				{
					sol::error e = r;
					Console.ErrorFmt("[Script] on_state_load error: {}", e.what());
				}
			}
		}

		// on_frame = GS-thread work: engine bookkeeping (cursor/nav/training/patches/camera/freecam/freeze) + the
		// stats HUD. The frame-perfect box geometry is captured separately on the EE thread (on_capture/RunCapture).
		if (s_haveOnFrame)
			RunProtected("on_frame", true);

		// on_gui = the script's ImGui window(s). Draw it in-game UNLESS the script popped it out -- then the
		// engine's separate popout window renders it (via RunGui()).
		if (s_haveOnGui && !s_popout.load())
			RunProtected("on_gui", true);
	}

	// Geometry pass on the EE/CPU thread (engine calls this from Counters::VSyncStart, when the frame's game state
	// is fully settled in eeMem -- BEFORE it ships to the GS). Runs on_capture() into the EE-thread prim list so the
	// boxes read from the EXACT frame about to be displayed. Pure reads + project.* + draw.*; no imgui/input.
	void RunCapture()
	{
		if (!s_enabled.load())
			return;
		std::lock_guard<std::mutex> lk(s_stateMtx); // serialise vs RunFrame / RunGui (GS thread)
		if (!s_state || !s_haveOnCapture)
			return;
		lua_sethook(s_state->lua_state(), InsnBudgetHook, LUA_MASKCOUNT, SDBZ_LUA_INSN_BUDGET);
		RunProtected("on_capture", true); // banner (if any) emits into the EE-thread prim list -> delivered + blitted
	}

	void RunGui()
	{
		// Called on the GS thread with an imgui context current -- either the in-game one (DrawScriptOverlay) or the
		// popout window's. Runs on_gui() into the current context.
		if (!s_enabled.load())
			return;
		std::lock_guard<std::mutex> lk(s_stateMtx); // serialise vs RunCapture (EE thread) / RunFrame
		if (!s_state || !s_haveOnGui)
			return;
		lua_sethook(s_state->lua_state(), InsnBudgetHook, LUA_MASKCOUNT, SDBZ_LUA_INSN_BUDGET);
		RunProtected("on_gui", false); // no in-game banner (the popout context has no game draw list)
	}

	void SetPopout(bool on) { s_popout.store(on); }
	bool WantsPopout() { return s_popout.load() && s_enabled.load() && s_haveOnGui; }

	const char* LastError()
	{
		return s_error.c_str();
	}

	void Dispatch(const char* name)
	{
		if (!name || !*name)
			return;
		std::lock_guard<std::mutex> lk(s_dispatchMutex);
		if (s_dispatchQueue.size() < 64) // cap so a stalled GS thread can't grow this unbounded
			s_dispatchQueue.emplace_back(name);
	}
} // namespace Script
