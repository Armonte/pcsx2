// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Lua scripting host for the in-process overlay. Owns a sol::state that hot-reloads a per-game script
// (scripts/<game>.lua; sdbz.lua is the prototype) without recompiling PCSX2. The script draws through ScriptBridge, so the
// game-specific knowledge (offsets, struct layout, colours) lives in Lua while the engine (EE-RAM
// access, projection, the pixel-exact draw pipeline, freeze/step) stays in compiled C++.
//
// The sol::state is created/run on the GS thread (RunFrame/RunGui) AND run on the EE/CPU thread (RunCapture);
// an internal mutex serialises the three so the single lua_State is never touched concurrently.
namespace Script
{
	bool        IsEnabled();          // Lua overlay path toggled on (Ctrl+L)
	bool        IsActive();           // enabled AND a script with on_frame() is loaded -> C++ box path stands down
	void        SetEnabled(bool on);  // toggle; first enable triggers a load
	void        Reload();             // force reload from disk (Ctrl+R)
	void        AutoEnableOnce();     // GS thread: honour [Script] AutoEnable once per session
	void        RunFrame();           // GS thread: poll hot-reload + on_frame (bookkeeping/HUD) + on_gui unless popped out
	void        RunCapture();         // EE/CPU thread (Counters::VSyncStart): on_capture() -- frame-perfect box geometry
	void        RunGui();             // run on_gui() in the CURRENT imgui context (the engine's popout window calls this)
	const char* LastError();          // "" when healthy

	// Pop-out: a script asks (engine.set_popout) for its on_gui() window in a separate OS window; the engine
	// (ScriptOverlay) creates that window and calls RunGui() into it. Game-agnostic -- works for ANY script.
	void        SetPopout(bool on);
	bool        WantsPopout();

	// Rebindable hotkeys (PCSX2 Settings->Hotkeys) call this from WHATEVER thread fires them. The name is queued
	// and drained on the GS thread inside RunFrame(), then delivered to the script's on_hotkey(name). Thread-safe.
	void        Dispatch(const char* name);
} // namespace Script
