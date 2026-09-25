// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Scriptable live overlay. Frame-perfect in-process overlay: the script's geometry pass runs on the EE/CPU
// thread at end-of-frame (CaptureOnEEThread), so boxes are read from the exact frame about to be displayed and
// handed to the GS thread in frame order; Draw() blits them at present time. Game knowledge lives in Lua; the
// whole C++ implementation lives in ScriptOverlay.cpp.
namespace ScriptOverlay
{
	// GS thread: called once per presented frame from ImGuiManager::RenderOverlays(). Runs the script's GS-thread
	// work (bookkeeping/HUD/UI) and blits the EE-captured geometry delivered by CaptureOnEEThread().
	void Draw();

	// EE/CPU thread: called from Counters::VSyncStart at end-of-frame (game state fully settled, before it ships to
	// the GS). Runs the script's on_capture() geometry pass and queues the screen-space prims for Draw() to blit, in
	// frame order -> the overlay reads at the right time and lines up with the displayed frame (no jitter/lag guess).
	void CaptureOnEEThread();
} // namespace ScriptOverlay
