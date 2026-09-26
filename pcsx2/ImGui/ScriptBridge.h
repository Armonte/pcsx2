// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once
#include <cstdint>

// Engine seam between the compiled C++ overlay (ScriptOverlay.cpp -- owns EE-RAM access, the projection
// matrix, and the pixel-exact prim/draw pipeline) and the Lua scripting host (ScriptHost.cpp -- owns
// sol2). The whole Lua API funnels through these free functions, so sol2's heavy headers stay isolated
// in one translation unit and the script never touches PCSX2 internals or the recompiler directly.
//
// Game-agnostic by design: nothing here knows it's SDBZ. All offsets / struct layouts / colours live in
// the .lua script. A second game (Fate/Unlimited Codes, arcade 246/256, ...) is just a different script
// against this same surface.
namespace ScriptBridge
{
	// --- EE RAM (data only; the host keeps writes out of code ranges) ---
	uint32_t Read32(uint32_t addr);
	uint16_t Read16(uint32_t addr);
	uint8_t  Read8(uint32_t addr);
	float    ReadF32(uint32_t addr);
	void     WriteData32(uint32_t addr, uint32_t value);
	void     ReadMatrix(uint32_t addr, float out16[16]); // 16 contiguous floats in the engine's row-major layout
	bool     MemReady();                                  // EE RAM mapped this frame

	// --- projection (uses the world->screen matrix the engine sampled THIS frame) ---
	void ApplyMatrix(const float M16[16], float x, float y, float z, float out3[3]); // row-vector * M (engine convention)
	bool WorldToScreen(float wx, float wy, float wz, float& sx, float& sy);           // false if behind camera / culled
	bool DisplayToScreen(float u, float v, float& sx, float& sy);                     // 0..1 display coords -> window pixels (HUD anchor)

	// --- draw (appended to THIS frame's prim list -> identical pipeline to the C++ path) ---
	void DrawSphere(const float W16[16], float cx, float cy, float cz, float r, uint32_t rgba); // UV sphere, bone matrix W
	void DrawSphereWorld(float cx, float cy, float cz, float r, uint32_t rgba);                  // UV sphere at a WORLD point (identity)
	void DrawLineWorld(float x1, float y1, float z1, float x2, float y2, float z2, uint32_t rgba, float thickness);
	// Native batched polygon-soup draw straight from EE memory (fast path for stage-collision meshes): read the
	// local->world matrix @matAddr ONCE, then for each of `faceCount` face-records (stride `recStride`) read its
	// vertex count (@+vcountOff), pool start index (@+startOff) and normal.y (@+normalOff), pull that many vec4
	// verts from `poolAddr` (stride 16), transform->project->emit the closed polygon. Colour per face by its
	// normal: |ny|>=ceilThresh -> floorRgba else wallRgba; an rgba of 0 skips that category. Reads LIVE memory each
	// call (no caching) so destructible geometry stays correct. Game knowledge (the addresses/offsets) stays script-side.
	void DrawMeshFaces(uint32_t matAddr, uint32_t poolAddr, uint32_t recsAddr, int faceCount, int recStride,
		int vcountOff, int startOff, int normalOff, uint32_t floorRgba, uint32_t wallRgba, float ceilThresh, float thickness);

	// Native walk+draw of a whole collision scene-subtree -- ONE call replaces the Lua per-mesh dispatch loop (the
	// remaining FPS sink at ~600 meshes / ~8000 lines). The SCRIPT supplies all offsets (game knowledge stays in
	// Lua); the engine just DFS-walks, transforms, projects, emits, reading LIVE EE memory every call (no cache, so
	// destructible geometry stays correct). Optimizations: shared-edge DEDUP + per-mesh vertex-projection cache.
	struct CollTreeDraw {
		uint32_t root;                          // subtree root scene node
		uint32_t floorRgba, wallRgba, obstRgba; // per-category colour; 0 => skip that category
		uint32_t brkGrid;                       // node+gridOff == this => BREAKABLE node -> drawn in obstRgba
		float ceilThresh, thick;                // |face normal.y| >= ceilThresh => floor/ceiling, else wall
		int childOff, sibOff, matOff, nameOff, gridOff; // scene-node: first-child, next-sibling, world-4x4, name, bound CHitGrid
		int headOff, ownerOff, nextOff;         // collision: node->mesh-head, mesh->owner backref, mesh->next
		int countOff, poolOff, recsOff, centerOff, radiusOff; // CHitMesh fields
		int recStride, vcountOff, startOff, normalOff;        // face-record layout
		const char* hidden;                     // comma-set of node names to skip (or null/empty)
	};
	void DrawCollisionTree(const CollTreeDraw& p);
	void DrawTextScreen(float sx, float sy, uint32_t rgba, const char* utf8); // screen-space
	void DrawLineScreen(float x0, float y0, float x1, float y1, uint32_t rgba, float thickness);     // screen-space 2D line
	void DrawCircleScreen(float cx, float cy, float r, uint32_t rgba, bool filled, float thickness); // screen-space 2D circle
	void DrawTriScreen(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t rgba, float depth); // 2D filled triangle; depth = painter sort key (larger = farther/drawn first)

	// --- per-frame projection source / overlay UI state (script-driven; the engine knows no game specifics) ---
	void SetWorldMatrix(uint32_t addr); // script picks the world->screen matrix address; engine samples 16 floats
	void SetCursorVisible(bool on);     // script reports if it wants an interactive window (drives cursor + run-idle)
	void SetFrameDelay(int frames);     // free-run draw delay 0..8: replay an older frame's prims (EE leads display)
	void SetGamepadNav(bool on);        // global ImGui gamepad nav on/off (off = the pad drives the game, not the UI)

	// === de-native enablers: writes / recompiler-safe code-patching / input ===
	// These let the SCRIPT own freeze/freecam/camera/training logic instead of native C++.
	void  WriteDataF32(uint32_t addr, float value);
	void  WriteData8(uint32_t addr, uint8_t value);
	void  PatchCode(uint32_t addr, uint32_t word, bool persistent = false); // save original once, write word,
	                                               // invalidate block (CPU thread); persistent = code cave, kept by UnpatchAll
	void  UnpatchCode(uint32_t addr);              // restore the saved original (CPU thread)
	void  UnpatchAll();                            // restore EVERY active patch (script reload/disable teardown)
	// Savestate integration (CPU thread): states are saved without patches; a load reconciles the registry.
	void  BeginStateSave();
	void  EndStateSave();
	void  OnStateLoaded();
	uint32_t StateLoadSerial();                    // increments on every state load (script host polls it)
	void  StateLoadStats(uint32_t& reapplied, uint32_t& kept, uint32_t& dropped);
	bool  KeyDown(int vk);                          // Windows VK_* currently held
	void  MouseDelta(float& dx, float& dy);
	void  MousePos(float& x, float& y);             // absolute ImGui cursor position (screen px) for 2D hit-testing
	bool  MouseDown(int button);                    // 0=L 1=R 2=M
	float MouseWheel();
	void  LookDelta(float& dx, float& dy);          // freecam look: RAW OS cursor delta while RMB held, recentered
	                                                // each frame (edge-free, infinite) -- 1:1 with the native feel
	bool  WantMouse();                              // ImGui currently wants the mouse (pointer over a script window)
	void  SetMouseClaimed(bool on);                 // script claims the cursor (e.g. over the gizmo) for THIS frame
	bool  MouseClaimed();                           // -> suppresses PCSX2's double-click-toggles-fullscreen on it

	// sphere render style (de-natives OverlaySphere's old old hardcoded reads -- script drives it from its own cfg)
	void SetSphereStyle(int segments, bool shaded, float fill_alpha, float line_thickness);
	// PCSX2 display config (live, on the GS thread)
	void SetAspect(int idx);            // AspectRatioType index
	int  GetAspect();
	void SetDeinterlace(int mode);      // GSInterlaceMode index
	int  GetDeinterlace();
	void SetInterlaceOffset(bool on);
	bool GetInterlaceOffset();
} // namespace ScriptBridge
