# SDBZ Lua Framework — De-native Grind Plan

SDBZ is the **prototype** for a generic PS2 Lua modding framework in PCSX2. Goal: **zero game-specific
native C++** — the C++ side is a generic engine exposing primitives; each game is a `.lua`. Work the list
**in order**; each item lands green + hot-reloadable where possible. Box-drawing + projection + freeze
already proven live (PINE). Build = `cmd.exe /c build_inc.bat` (Windows; close PCSX2 first for the link).

## Engine API (host: ScriptHost.cpp, seam: ScriptBridge.h) — GAME-AGNOSTIC ONLY (no SDBZ knowledge in C++)
- `memory.read_u32/u16/u8/f32`, `write_u32/f32/u8`, `valid`, `ready`
- `project.world_to_screen / apply_matrix_at / display_to_screen / set_world_matrix(addr)`
- `draw.sphere(matAddr,..) / sphere_world / line_world / text / set_sphere_style`
- `engine.patch/unpatch` (recompiler-safe code NOPs), `set_cursor(bool)`,
  `set_aspect/get_aspect / set_deinterlace/get_deinterlace / set_interlace_offset/get_interlace_offset`
- `input.key_down(vk)/mouse_delta/mouse_down/wheel`
- `imgui.Begin/End/Text/Checkbox/Button/SliderInt/SliderFloat/ColorEdit4/Separator/SameLine/Spacing/TreeNode/TreePop/CalcTextWidth/GetFontSize`
- script callbacks the host calls: `on_frame()`, `on_hotkey(name)`
- globals `SCRIPT_DIR/SCRIPT_PATH`; `package.path` = `<dir>/?.lua;<dir>/lib/?.lua` (shared `lib/`)
- NOTE: players / battle-state / freeze / freecam / colours are NO LONGER in C++ — the script owns them via the
  primitives above. (Deleted bridge: get_player/player_valid/in_battle/flag/color/is_frozen/freeze/step/freecam.)
- hotkeys (Settings→Hotkeys "Script Overlay", unbound by default): Toggle Overlay / Reload / Toggle Window /
  Toggle Sim Freeze / Frame Step / Toggle Freecam → on_hotkey("toggle_window/toggle_freeze/frame_step/toggle_freecam")

## Menu parity grind (in order)
- [x] **Box types + 12 colors + layer toggles** — DONE (Lua)
- [x] **Framestep** (Freeze/Step/+10) — DONE (pure Lua; full freeze module owns bit 0x4 @0x5D4F90 + timer NOP
      @0x3EA56C + swing/cloth NOPs @0x362A38/48 + shadow-byte poke + step, all via `engine.patch`/`memory`).
- [x] **Training** — DONE (pure Lua; `in_battle()` gated, locks via `memory.write_*`, move = `player+48`).
- [x] **Hide game HUD** — DONE (`engine.patch` 0x309280 jr$ra;nop).
- [x] **Camera-scale / ortho** — DONE (`memory.write_f32` cam+0x10; ortho builds proj matrix).
- [x] **No-cull** — DONE (`engine.patch` 0x1C44F0 jr$ra;li$v0,1).
- [x] **Style** — DONE (`draw.set_sphere_style` bridge).
- [x] **Aspect ratio / Deinterlace** — DONE (`engine.set_aspect/set_deinterlace/set_interlace_offset`).
- [x] **Freecam** — DONE (pure Lua). `freecam_set_enabled` patches the 6 eye/lookat stores
      (FC_STORES 0x2B4E1C/E24/E28/E54/E58/E5C) to nop via `engine.patch`; `freecam_update` integrates
      `input.*`, writes eye@cam+0x150/lookat@cam+0x160/view@cam+0x50 + rebuilds cull frustum (cam+208/560,
      RowMul inline). No new C++ — used existing bridge. Toggled from the Lua control window's Freecam section.
- [x] **Frame delay** — DONE. Engine keeps the 9-deep prim ring + `engine.set_frame_delay(n)` (0..8, forced 0 when
      paused); script owns `cfg.frame_delay` (**default 2 = the tuned value we ran at**) + a slider. EE RAM leads the
      displayed frame even in-process, so this lines the boxes up with the action; replays an older frame's prims.

## Then (coupled final pass) — ALL DONE ✅
- [x] **Hotkey→Lua**: `Script::Dispatch(name)` thread-safe (mutex) queue in ScriptHost; hotkey pushes any thread →
      GS-thread `RunFrame` drains via `DrainDispatch()` → lua `on_hotkey(name)`. `Hotkeys.cpp` routes
      ScriptToggleFreeze/FrameStep/ToggleFreecam → `Script::Dispatch("toggle_freeze"/"frame_step"/"toggle_freecam")`.
- [x] **DELETE native SDBZ C++**: `SdbzOverlay.cpp` 1637 → 363 lines. Removed all Sdbz* game logic
      (settings/colors/save-load, Draw*/ModelObj, freecam, framestep/freeze, training, hudhide, nocull, camscale,
      ortho, control body/window/popout, GetAsyncKeyState block) + bridge Flag/Color/IsInBattle/GetPlayer/
      PlayerValid/IsFrozen/Freeze/Step/ToggleFreecam + their host bindings. **KEPT** generic engine:
      Ee*/EeMat/RowMul/WorldToWindow/SdbzPrim/Emit*/SdbzSphere/SdbzBlit + g_sphere_style + a tiny generic frame
      driver + ScriptBridge + host. Added generic `project.set_world_matrix(addr)` (script picks the WS source)
      + `engine.set_cursor(bool)` (cursor + run-idle). **Build GREEN (INC_BUILD_EXITCODE=0).**
- [x] **Extract `lib/`** — `scripts/lib/{color,persist,ui}.lua` `require()`d by sdbz.lua (host package.path already
      had `<dir>/lib/?.lua`). Validated via lua5.4 syntax + a require/load + 10-frame/7-hotkey runtime smoke test.

## After SDBZ is fully de-natived (the prototype is proven)
- [ ] Template a `template.lua` / document the API.
- [ ] Port the framework into **pcsx2x6** (`/mnt/c/dev/pcsx2x6`): drop the engine files + redo the 8:7 aspect
      patch (AspectRatioType moved to Config.h there). Then `fuc.lua` (Fate/Unlimited Codes), arcade 246/256.
