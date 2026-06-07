// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "BuildVersion.h"
#include "Config.h"
#include "Counters.h"
#include "Memory.h" // SDBZ hitbox overlay: live EE RAM via eeMem
#include "R5900.h" // SDBZ freecam: Cpu->Clear (recompiler invalidate for code NOP patch)
#include "GS/GS.h"
#include "GS/GSShaderCompileIndicator.h"
#include "GS/GSCapture.h"
#include "GS/GSVector.h"
#include "GS/Renderers/Common/GSDevice.h"
#ifdef _WIN32
#include "GS/Renderers/DX12/GSDevice12.h"
#include "common/RedtapeWindows.h" // SDBZ overlay toggle: GetAsyncKeyState
#endif
#include "GS/Renderers/HW/GSTextureReplacements.h"
#include "Host.h"
#include "IconsFontAwesome.h"
#include "IconsPromptFont.h"
#include "ImGui/FullscreenUI.h"
#include "ImGui/ImGuiAnimated.h"
#include "ImGui/ImGuiFullscreen.h"
#include "ImGui/ImGuiManager.h"
#include "ImGui/ImGuiOverlays.h"
#include "Input/InputManager.h"
#include "MTGS.h"
#include "Patch.h"
#include "PerformanceMetrics.h"
#include "Recording/InputRecording.h"
#include "SIO/Pad/Pad.h"
#include "SIO/Pad/PadBase.h"
#include "USB/USB.h"
#include "VMManager.h"

#include "common/BitUtils.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/Timer.h"

#include "fmt/chrono.h"
#include "fmt/format.h"
#include "imgui.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <tuple>
#include <vector>

#ifdef _WIN32
// SDBZ control-panel popout: a self-contained separate OS window (own ImGui context + Win32 window + a small
// standalone D3D11 device/swapchain), decoupled from the game's GS renderer (works on Vulkan/DX/GL).
#include <d3d11.h>
#include "ImGui/backends/imgui_impl_win32.h"
#include "ImGui/backends/imgui_impl_dx11.h"
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
#endif
#include "ImGui/SdbzOverlay.h"

// ============================== SDBZ live hitbox overlay ==============================
// Frame-perfect because it draws in-process at present time from live EE RAM. SDBZ retail
// (SLUS-214.42). See sdbz_camera / sdbz_livehit (Python) for the RE'd offsets it mirrors.
namespace
{
	static inline u32 Ee32(u32 a) { return eeMem ? *reinterpret_cast<const u32*>(&eeMem->Main[a & 0x01FFFFFFu]) : 0u; }
	static inline u16 Ee16(u32 a) { return eeMem ? *reinterpret_cast<const u16*>(&eeMem->Main[a & 0x01FFFFFFu]) : 0u; }
	static inline float EeF32(u32 a) { union { u32 u; float f; } c; c.u = Ee32(a); return c.f; }
	static inline bool EeValid(u32 p) { return p >= 0x100000u && p < 0x2000000u; }
	static void EeMat(u32 a, float m[16]) { for (int i = 0; i < 16; i++) m[i] = EeF32(a + 4u * i); }
	// raw EE data writes (freecam eye/lookat). DATA only -- never patch code via these (no recompiler
	// invalidation); code patches go through memWrite32 + Cpu->Clear on the CPU thread.
	static inline void EeStore32(u32 a, u32 v) { if (eeMem) *reinterpret_cast<u32*>(&eeMem->Main[a & 0x01FFFFFFu]) = v; }
	static inline void EeStoreF(u32 a, float f) { union { u32 u; float f; } c; c.f = f; EeStore32(a, c.u); }
		[[maybe_unused]] static inline void EeStore8(u32 a, u8 v) { if (eeMem) eeMem->Main[a & 0x01FFFFFFu] = v; }
	// row-vector point * 4x4
	static void RowMul(const float p[4], const float M[16], float out[4]) {
		for (int j = 0; j < 4; j++) out[j] = p[0] * M[j] + p[1] * M[4 + j] + p[2] * M[8 + j] + p[3] * M[12 + j];
	}
	constexpr float SDBZ_RENDER_W = 512.0f, SDBZ_RENDER_H = 448.0f;
	constexpr u32 SDBZ_PLAYER_VT = 0x4f2440u, SDBZ_WS = 0x508f90u, SDBZ_REMAP = 0x4a20a0u;

	// world point -> window pixels via g_WorldToScreen (render 512x448 -> displayed image rect).
	// CRITICAL: reject NaN/Inf/out-of-range. Feeding garbage vertices to ImGui->GS corrupts the
	// render pass (black frame). pygame ignored bad coords; the GPU does not.
	static bool WorldToWindow(const float WS[16], float wx, float wy, float wz, ImVec2& out) {
		const float p[4] = {wx, wy, wz, 1.0f}; float clip[4]; RowMul(p, WS, clip);
		if (!(clip[3] > 0.0001f)) return false; // also rejects NaN (NaN > x is false)
		const float rx = clip[0] / clip[3], ry = clip[1] / clip[3];
		if (!std::isfinite(rx) || !std::isfinite(ry)) return false;
		float winx, winy;
		GSTranslateDisplayToWindowCoordinates(rx / SDBZ_RENDER_W, ry / SDBZ_RENDER_H, &winx, &winy);
		if (!std::isfinite(winx) || !std::isfinite(winy)) return false;
		if (winx < -20000.0f || winx > 20000.0f || winy < -20000.0f || winy > 20000.0f) return false;
		out = ImVec2(winx, winy); return true;
	}

	// the model object a player's posed bones share (mode of node+440 over bone nodes)
	static u32 SdbzModelObj(u32 player) {
		u32 stack[2048]; int sp = 0; u32 fc = Ee32(player + 16); if (EeValid(fc)) stack[sp++] = fc;
		u32 vals[64]; int cnt[64]; int nv = 0, guard = 0;
		while (sp > 0 && guard++ < 4096) {
			u32 n = stack[--sp]; s32 bi = static_cast<s32>(Ee32(n + 436)); u32 mo = Ee32(n + 440);
			if (bi >= 0 && bi < 200 && EeValid(mo)) {
				int k; for (k = 0; k < nv; k++) if (vals[k] == mo) { cnt[k]++; break; }
				if (k == nv && nv < 64) { vals[nv] = mo; cnt[nv] = 1; nv++; }
			}
			u32 c = Ee32(n + 16); if (EeValid(c) && sp < 2047) stack[sp++] = c;
			u32 s = Ee32(n + 8); if (EeValid(s) && sp < 2047) stack[sp++] = s;
		}
		int best = -1; u32 bo = 0; for (int k = 0; k < nv; k++) if (cnt[k] > best) { best = cnt[k]; bo = vals[k]; }
		return bo;
	}

	// ================= SDBZ overlay user settings (control window + JSON persistence) =================
	struct SdbzColor { float r, g, b, a; };
	struct SdbzSettings {
		bool master = true;
		bool show_hurt = true, show_attack = true, show_throw = true,
			 show_prox = true, show_other = true, show_skel = false, show_state = true, show_shells = true;
		bool hide_hud = false; // hide the game's HUD (combo/bars/timer) -- clears the scene-node +56 "always-on" flag
		SdbzColor c_hurt   {0.24f, 0.86f, 0.35f, 0.70f};
		SdbzColor c_attack {1.00f, 0.24f, 0.24f, 0.90f};
		SdbzColor c_throw  {0.86f, 0.27f, 0.86f, 0.86f};
		SdbzColor c_prox   {1.00f, 0.78f, 0.16f, 0.74f};
		SdbzColor c_other  {0.27f, 0.59f, 0.92f, 0.59f};
		SdbzColor c_skel   {0.47f, 0.90f, 0.47f, 0.78f};
		SdbzColor c_shell  {1.00f, 0.50f, 0.10f, 0.85f};
		float box_thickness = 1.5f;
		float skel_thickness = 1.0f;
		int   sphere_segments = 20;     // UV-sphere resolution (latitude/longitude divisions)
		bool  sphere_shaded = false;    // translucent shaded fill (lit) vs wireframe only
		float sphere_fill_alpha = 0.30f; // fill opacity multiplier when shaded
		int   frame_delay = 0;          // free-run EE-ahead compensation (0..8 frames); ignored when paused
		bool  cam_hscale_on = false;    // master toggle for the live camera-matrix override (h-scale + zoom)
		float cam_hscale = 0.75f;       // horizontal scale: game default 0.75; ~0.875 = 1:1 on Native-8:7
		float cam_zoom = 1.0f;          // FOV zoom: 1.0 = default, <1 = zoom out (wider), >1 = zoom in
		bool  cam_link_zoom = false;    // link zoom to h-scale (zoom = 0.75/h-scale -> widening zooms out)
		bool  show_window = false;      // control window visibility (toggle: Ctrl+J)
		bool  popout = false;           // render the control panel in a separate OS window (not persisted)
	};
	static SdbzSettings g_sdbz;

	// Freecam state (declared here so JSON save/load can persist the tuning). Logic is further below.
	struct SdbzFreecam {
		bool enabled = false;
		float eye[3] = {0, 0, 0};
		float yaw = 0.0f, pitch = 0.0f;   // radians; forward = (cosY*cosP, -sinP, sinY*cosP), Y is world up
		float pend_yaw = 0.0f, pend_pitch = 0.0f; // un-applied look input, eased in for smoothing
		float move_speed = 3.0f;          // world units / 60fps-frame at 1x (Shift=4x, Ctrl=0.25x)
		float look_speed = 0.03f;         // radians / frame (arrow keys)
		float mouse_sens = 0.0030f;       // radians / raw cursor pixel (RMB mouse-look)
		float look_smooth = 0.55f;        // 0 = instant/raw, ->1 = floaty (s&box-like ease-out)
		bool invert_y = false;            // pitch direction for mouse-look
		u32 saved[6] = {0, 0, 0, 0, 0, 0}; // original instruction words (restored on disable)
	};
	static SdbzFreecam g_fc;

	static inline ImU32 SdbzCol(const SdbzColor& c) { return ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, c.a)); }

	static std::string SdbzSettingsPath() { return Path::Combine(EmuFolders::Settings, "sdbz_overlay.json"); }

	static void SdbzSaveSettings() {
		const SdbzSettings& s = g_sdbz;
		std::string j = "{\n";
		auto b = [&](const char* k, bool v) { j += fmt::format("  \"{}\": {},\n", k, v ? "true" : "false"); };
		auto f = [&](const char* k, float v) { j += fmt::format("  \"{}\": {:.4f},\n", k, v); };
		auto i = [&](const char* k, int v) { j += fmt::format("  \"{}\": {},\n", k, v); };
		auto c = [&](const char* k, const SdbzColor& v) { j += fmt::format("  \"{}\": [{:.4f}, {:.4f}, {:.4f}, {:.4f}],\n", k, v.r, v.g, v.b, v.a); };
		b("master", s.master);
		b("show_hurt", s.show_hurt); b("show_attack", s.show_attack); b("show_throw", s.show_throw);
		b("show_prox", s.show_prox); b("show_other", s.show_other); b("show_skel", s.show_skel); b("show_state", s.show_state);
		b("hide_hud", s.hide_hud);
		b("show_shells", s.show_shells);
		c("c_hurt", s.c_hurt); c("c_attack", s.c_attack); c("c_throw", s.c_throw);
		c("c_prox", s.c_prox); c("c_other", s.c_other); c("c_skel", s.c_skel); c("c_shell", s.c_shell);
		f("box_thickness", s.box_thickness); f("skel_thickness", s.skel_thickness);
		i("sphere_segments", s.sphere_segments); i("frame_delay", s.frame_delay);
		b("sphere_shaded", s.sphere_shaded); f("sphere_fill_alpha", s.sphere_fill_alpha);
		b("cam_hscale_on", s.cam_hscale_on); f("cam_hscale", s.cam_hscale); f("cam_zoom", s.cam_zoom);
		b("cam_link_zoom", s.cam_link_zoom);
		f("fc_move_speed", g_fc.move_speed); f("fc_mouse_sens", g_fc.mouse_sens);
		f("fc_look_speed", g_fc.look_speed); f("fc_look_smooth", g_fc.look_smooth);
		b("fc_invert_y", g_fc.invert_y);
		j += "  \"_version\": 1\n}\n";
		FileSystem::WriteStringToFile(SdbzSettingsPath().c_str(), j);
	}

	// Lenient flat-JSON reader: finds "key" then the next number(s)/bool after the ':'. Tolerates spacing,
	// ordering, and missing keys (those keep their defaults). Not a general parser -- only our flat object.
	static void SdbzLoadSettings() {
		const auto data = FileSystem::ReadFileToString(SdbzSettingsPath().c_str());
		if (!data.has_value()) return; // first run -> defaults
		const std::string& t = data.value();
		SdbzSettings& s = g_sdbz;
		auto find_val = [&](const char* key) -> size_t {
			const std::string pat = std::string("\"") + key + "\"";
			size_t p = t.find(pat); if (p == std::string::npos) return std::string::npos;
			p = t.find(':', p + pat.size()); return (p == std::string::npos) ? std::string::npos : p + 1;
		};
		auto rd_b = [&](const char* k, bool& v) { size_t p = find_val(k); if (p != std::string::npos) v = (t.compare(t.find_first_not_of(" \t", p), 4, "true") == 0); };
		auto rd_f = [&](const char* k, float& v) { size_t p = find_val(k); if (p != std::string::npos) v = std::strtof(t.c_str() + p, nullptr); };
		auto rd_i = [&](const char* k, int& v) { size_t p = find_val(k); if (p != std::string::npos) v = static_cast<int>(std::strtol(t.c_str() + p, nullptr, 10)); };
		auto rd_c = [&](const char* k, SdbzColor& v) {
			size_t p = find_val(k); if (p == std::string::npos) return;
			p = t.find('[', p); if (p == std::string::npos) return;
			const char* q = t.c_str() + p + 1; char* end = nullptr;
			v.r = std::strtof(q, &end); v.g = std::strtof(end + 1, &end); v.b = std::strtof(end + 1, &end); v.a = std::strtof(end + 1, &end);
		};
		rd_b("master", s.master);
		rd_b("show_hurt", s.show_hurt); rd_b("show_attack", s.show_attack); rd_b("show_throw", s.show_throw);
		rd_b("show_prox", s.show_prox); rd_b("show_other", s.show_other); rd_b("show_skel", s.show_skel); rd_b("show_state", s.show_state);
		rd_b("hide_hud", s.hide_hud);
		rd_b("show_shells", s.show_shells);
		rd_c("c_hurt", s.c_hurt); rd_c("c_attack", s.c_attack); rd_c("c_throw", s.c_throw);
		rd_c("c_prox", s.c_prox); rd_c("c_other", s.c_other); rd_c("c_skel", s.c_skel); rd_c("c_shell", s.c_shell);
		rd_f("box_thickness", s.box_thickness); rd_f("skel_thickness", s.skel_thickness);
		rd_i("sphere_segments", s.sphere_segments); rd_i("frame_delay", s.frame_delay);
		rd_b("sphere_shaded", s.sphere_shaded); rd_f("sphere_fill_alpha", s.sphere_fill_alpha);
		rd_b("cam_hscale_on", s.cam_hscale_on); rd_f("cam_hscale", s.cam_hscale); rd_f("cam_zoom", s.cam_zoom);
		rd_b("cam_link_zoom", s.cam_link_zoom);
		rd_f("fc_move_speed", g_fc.move_speed); rd_f("fc_mouse_sens", g_fc.mouse_sens);
		rd_f("fc_look_speed", g_fc.look_speed); rd_f("fc_look_smooth", g_fc.look_smooth);
		rd_b("fc_invert_y", g_fc.invert_y);
		if (s.sphere_segments < 4) s.sphere_segments = 4; if (s.sphere_segments > 48) s.sphere_segments = 48;
		if (s.frame_delay < 0) s.frame_delay = 0; if (s.frame_delay > 8) s.frame_delay = 8;
	}

	// One overlay primitive. We render into a per-frame list, then push it through a small ring buffer so
	// the free-run "frame_delay" can draw an OLDER frame's geometry (EE RAM runs ~1 frame ahead of display).
	struct SdbzPrim {
		int kind;       // 0 = line, 1 = text, 2 = filled triangle (a,b,c)
		ImVec2 a, b;    // line endpoints; for text a = position
		ImU32 col;
		float thick;
		char text[28];
		ImVec2 c;       // 3rd vertex for filled triangles
	};
	static inline void EmitLine(std::vector<SdbzPrim>& o, ImVec2 a, ImVec2 b, ImU32 col, float th) {
		o.push_back(SdbzPrim{0, a, b, col, th, {0}});
	}
	static inline void EmitText(std::vector<SdbzPrim>& o, ImVec2 a, ImU32 col, const char* s) {
		SdbzPrim p{1, a, ImVec2(0, 0), col, 0.0f, {0}}; std::snprintf(p.text, sizeof(p.text), "%s", s); o.push_back(p);
	}
	static inline void EmitTri(std::vector<SdbzPrim>& o, ImVec2 a, ImVec2 b, ImVec2 c, ImU32 col, float depth) {
		SdbzPrim p{2, a, b, col, depth, {0}}; p.c = c; o.push_back(p); // thick field carries depth for sorting
	}

	// Proper UV sphere (latitude/longitude mesh), replacing the old 3-great-circle look. Optional translucent
	// SHADED FILL: lit by a fixed light, painter's-sorted back-to-front (correct for a convex sphere). W =
	// local->world matrix (a bone matrix, or identity for world-space shells); center/r are in that frame.
	static void SdbzSphere(std::vector<SdbzPrim>& out, const float WS[16], const float W[16],
	                       float cx, float cy, float cz, float r, ImU32 col) {
		constexpr int MAXST = 22, MAXSL = 42;
		int ST = g_sdbz.sphere_segments / 2; if (ST < 3) ST = 3; if (ST > MAXST - 1) ST = MAXST - 1; // latitude
		int SL = g_sdbz.sphere_segments;     if (SL < 6) SL = 6; if (SL > MAXSL - 1) SL = MAXSL - 1; // longitude
		static ImVec2 scr[MAXST][MAXSL]; static float dep[MAXST][MAXSL]; static bool ok[MAXST][MAXSL];
		static float nrm[MAXST][MAXSL][3]; // GS thread only, fully rewritten before read each call
		for (int i = 0; i <= ST; i++) {
			const float th = 3.14159265f * static_cast<float>(i) / static_cast<float>(ST);
			const float ct = std::cos(th), stt = std::sin(th);
			for (int j = 0; j <= SL; j++) {
				const float ph = 6.28318531f * static_cast<float>(j) / static_cast<float>(SL);
				const float lnx = stt * std::cos(ph), lny = ct, lnz = stt * std::sin(ph); // local unit normal
				const float lp[4] = {cx + r * lnx, cy + r * lny, cz + r * lnz, 1.0f};
				float wp[4]; RowMul(lp, W, wp);
				ImVec2 sv; ok[i][j] = WorldToWindow(WS, wp[0], wp[1], wp[2], sv); scr[i][j] = sv;
				const float ln4[4] = {lnx, lny, lnz, 0.0f}; float wn[4]; RowMul(ln4, W, wn);
				nrm[i][j][0] = wn[0]; nrm[i][j][1] = wn[1]; nrm[i][j][2] = wn[2];
				dep[i][j] = wp[0] * WS[3] + wp[1] * WS[7] + wp[2] * WS[11] + WS[15]; // clip w = depth
			}
		}
		if (g_sdbz.sphere_shaded) {
			const float lx = 0.50f, ly = 0.70f, lz = 0.51f; // fixed light direction
			const int aI = static_cast<int>(static_cast<float>((col >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f * g_sdbz.sphere_fill_alpha * 255.0f);
			const int br = (col >> IM_COL32_R_SHIFT) & 0xFF, bg = (col >> IM_COL32_G_SHIFT) & 0xFF, bb = (col >> IM_COL32_B_SHIFT) & 0xFF;
			// Emit each quad's 2 tris with their depth (clip w). SdbzBlit globally depth-sorts ALL fill tris
			// across every sphere (painter's, far->near) so overlapping body capsules layer correctly.
			for (int i = 0; i < ST; i++) for (int j = 0; j < SL; j++) {
				if (!ok[i][j] || !ok[i + 1][j] || !ok[i + 1][j + 1] || !ok[i][j + 1]) continue;
				const float fnx = nrm[i][j][0] + nrm[i + 1][j][0] + nrm[i + 1][j + 1][0] + nrm[i][j + 1][0];
				const float fny = nrm[i][j][1] + nrm[i + 1][j][1] + nrm[i + 1][j + 1][1] + nrm[i][j + 1][1];
				const float fnz = nrm[i][j][2] + nrm[i + 1][j][2] + nrm[i + 1][j + 1][2] + nrm[i][j + 1][2];
				const float fl = std::sqrt(fnx * fnx + fny * fny + fnz * fnz); if (fl < 1e-6f) continue;
				float ndl = (fnx * lx + fny * ly + fnz * lz) / fl; if (ndl < 0.0f) ndl = 0.0f;
				const float in = 0.40f + 0.60f * ndl; // ambient + diffuse
				const float qd = 0.25f * (dep[i][j] + dep[i + 1][j] + dep[i + 1][j + 1] + dep[i][j + 1]);
				const ImU32 qc = IM_COL32(static_cast<int>(br * in), static_cast<int>(bg * in), static_cast<int>(bb * in), aI);
				EmitTri(out, scr[i][j], scr[i + 1][j], scr[i + 1][j + 1], qc, qd);
				EmitTri(out, scr[i][j], scr[i + 1][j + 1], scr[i][j + 1], qc, qd);
			}
		}
		const float wth = g_sdbz.box_thickness;
		for (int i = 1; i < ST; i++) for (int j = 0; j < SL; j++)
			if (ok[i][j] && ok[i][j + 1]) EmitLine(out, scr[i][j], scr[i][j + 1], col, wth); // latitude rings
		for (int j = 0; j < SL; j++) for (int i = 0; i < ST; i++)
			if (ok[i][j] && ok[i + 1][j]) EmitLine(out, scr[i][j], scr[i + 1][j], col, wth); // longitude lines
	}

	static void SdbzDrawSkeleton(std::vector<SdbzPrim>& out, u32 player, const float WS[16]) {
		const u32 mo = SdbzModelObj(player); if (!EeValid(mo)) return;
		// a "good bone" is a real posed bone (right model object + valid index) that is NOT sitting at
		// world origin (unposed bones read world Y ~0; every real posed bone has Y well above ground).
		auto goodBone = [&](u32 n) {
			if (!EeValid(n) || Ee32(n + 440) != mo) return false;
			s32 bi = static_cast<s32>(Ee32(n + 436)); if (bi < 0 || bi >= 200) return false;
			const float y = EeF32(n + 292); return std::isfinite(y) && y > 2.0f;
		};
		u32 stack[2048]; int sp = 0; u32 fc = Ee32(player + 16); if (EeValid(fc)) stack[sp++] = fc;
		int guard = 0;
		while (sp > 0 && guard++ < 4096) {
			u32 n = stack[--sp];
			if (goodBone(n)) {
				// connect to nearest GOOD bone ancestor (skip helpers AND origin/unposed bones)
				u32 par = Ee32(n + 12); int g = 0; while (EeValid(par) && !goodBone(par) && g++ < 48) par = Ee32(par + 12);
				ImVec2 a, b;
				// adjacent posed bones are always close in world space; a long segment means a line to
				// a stray/origin bone -> skip it (robust catch-all for the "stretched to origin" lines).
				const float dx = EeF32(n + 288) - EeF32(par + 288), dy = EeF32(n + 292) - EeF32(par + 292), dz = EeF32(n + 296) - EeF32(par + 296);
				if (goodBone(par) && (dx * dx + dy * dy + dz * dz) < 225.0f /* < 15 world units */
					&& WorldToWindow(WS, EeF32(n + 288), EeF32(n + 292), EeF32(n + 296), a)
					&& WorldToWindow(WS, EeF32(par + 288), EeF32(par + 292), EeF32(par + 296), b))
					EmitLine(out, a, b, SdbzCol(g_sdbz.c_skel), g_sdbz.skel_thickness);
			}
			u32 c = Ee32(n + 16); if (EeValid(c) && sp < 2047) stack[sp++] = c;
			u32 s = Ee32(n + 8); if (EeValid(s) && sp < 2047) stack[sp++] = s;
		}
	}

	static void SdbzDrawBoxes(std::vector<SdbzPrim>& out, u32 player, const float WS[16]) {
		const u32 skel = Ee32(player + 1968); if (!EeValid(skel)) return;
		// place a box by its cht bone index (cht bone -> g_ChtBoneRemap -> skeleton node -> world@+240),
		// drawn as a real UV sphere (SdbzSphere: latitude/longitude mesh + optional shaded fill).
		auto boneSphere = [&](u32 bone, float cx, float cy, float cz, float r, ImU32 col) {
			if (r <= 0.0f || bone >= 23u) return;
			const u32 node = Ee32(skel + 12u + 4u * Ee32(SDBZ_REMAP + 4u * bone)); if (!EeValid(node)) return;
			float W[16]; EeMat(node + 240, W);
			SdbzSphere(out, WS, W, cx, cy, cz, r, col);
		};
		// --- HURT capsules (always-on): the FULL body-box table, not just the runtime body-part nodes.
		// The table has multiple capsules per bone (limb root + mid + end -> elbow/hand, knee/foot).
		// Base = lowest box addr among the body-part nodes (player+6056+4*b); scan 32B stride to radius<=0.
		if (g_sdbz.show_hurt) {
			u32 tbase = 0;
			for (int b = 0; b < 9; b++) {
				const u32 node = Ee32(player + 6056u + 4u * b); if (!EeValid(node)) continue;
				const u32 box = Ee32(node + 20); if (EeValid(box) && (tbase == 0 || box < tbase)) tbase = box;
			}
			if (EeValid(tbase)) {
				const ImU32 col = SdbzCol(g_sdbz.c_hurt);
				for (int k = 0; k < 24; k++) {
					const u32 a = tbase + 32u * k; const float r = EeF32(a + 16);
					if (!(r > 0.0f)) break; // end of contiguous table
					boneSphere(Ee32(a + 20), EeF32(a), EeF32(a + 4), EeF32(a + 8), r, col);
				}
			}
		}
		// --- ACTIVE move boxes (attack/throw/prox) from the current hit group, frame-filtered ---
		const u32 grp = Ee32(player + 6204), cm = Ee32(player + 1976);
		if (!EeValid(grp) || !EeValid(cm)) return;
		const s32 frame = static_cast<s32>(Ee32(cm + 152));
		auto entry = [&](u32 he, ImU32 col) {
			const u16 s = Ee16(he + 0x20), e = Ee16(he + 0x22);
			if (frame < static_cast<s32>(s) || frame > static_cast<s32>(e)) return;
			for (int j = 0; j < 8; j++) {
				const u32 bp = Ee32(he + 0x24 + 4u * j); if (!EeValid(bp)) continue;
				boneSphere(Ee32(bp + 20), EeF32(bp), EeF32(bp + 4), EeF32(bp + 8), EeF32(bp + 16), col);
			}
		};
		for (int i = 0; i < 16; i++) { const u32 he = Ee32(grp + 8 + 4u * i); if (!EeValid(he)) continue;
			const u32 mask = Ee32(he + 0x1c);
			const bool is_throw = (mask & 0x480u) != 0;
			const bool is_attack = !is_throw && (mask & 0x9000D80u) == 0;
			if (is_throw) { if (g_sdbz.show_throw) entry(he, SdbzCol(g_sdbz.c_throw)); }      // throw / command grab
			else if (is_attack) { if (g_sdbz.show_attack) entry(he, SdbzCol(g_sdbz.c_attack)); } // attack
			else { if (g_sdbz.show_other) entry(he, SdbzCol(g_sdbz.c_other)); } }              // other / volume
		if (g_sdbz.show_prox)
			for (int i = 0; i < 8; i++) { const u32 he = Ee32(grp + 0x68 + 4u * i); if (!EeValid(he)) continue;
				entry(he, SdbzCol(g_sdbz.c_prox)); } // proximity / guard-trigger
	}

	// Projectile/shell hitboxes. Each player owns 16 shell slots @player+1820+4*i; active when shell+832 & 8
	// (Player_AllocFreeShell@0x1CD840). The hit volume is a capsule CHAIN: segments @*(shell+1376) (48B stride,
	// world point A@+0, world point B@+16 -- the live swept positions), count = *(*(shell+1384)+12), radius in
	// the param buffer *(shell+1388) (32B stride, +16). Ki-blast = 1 seg (sphere), beam = N segs (chain). The
	// builders (CShlSp254_UpdateHitbox@0x3A64F0, CShlCtrlKame_UpdateBeamHitboxes@0x3B3AE0) write world-space.
	static void SdbzDrawShells(std::vector<SdbzPrim>& out, u32 player, const float WS[16]) {
		const ImU32 col = SdbzCol(g_sdbz.c_shell);
		const float ident[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}; // shell positions are already world-space
		for (int i = 0; i < 16; i++) {
			const u32 shell = Ee32(player + 1820u + 4u * i);
			if (!EeValid(shell) || (Ee32(shell + 832) & 8u) == 0) continue; // slot empty / inactive
			const u32 ho = Ee32(shell + 1384), segbuf = Ee32(shell + 1376), parbuf = Ee32(shell + 1388);
			if (!EeValid(segbuf)) continue;
			s32 count = EeValid(ho) ? static_cast<s32>(Ee32(ho + 12)) : 1;
			if (count < 1) count = 1; if (count > 64) count = 64;
			for (int k = 0; k < count; k++) {
				const u32 seg = segbuf + 48u * static_cast<u32>(k);
				const float bx = EeF32(seg + 16), by = EeF32(seg + 20), bz = EeF32(seg + 24); // world point B (live tip)
				if (!std::isfinite(bx) || !std::isfinite(by) || !std::isfinite(bz)) continue;
				float r = 1.5f;
				if (EeValid(parbuf)) { const float pr = EeF32(parbuf + 32u * static_cast<u32>(k) + 16); if (pr > 0.05f && pr < 300.0f) r = pr; }
				SdbzSphere(out, WS, ident, bx, by, bz, r, col);
				const float ax = EeF32(seg), ay = EeF32(seg + 4), az = EeF32(seg + 8); // world point A (sweep start)
				ImVec2 pa, pb;
				if (WorldToWindow(WS, ax, ay, az, pa) && WorldToWindow(WS, bx, by, bz, pb)) EmitLine(out, pa, pb, col, g_sdbz.box_thickness);
			}
		}
	}

	// player state-flag HUD: parry / invuln / guard-point (player+1312, Player_SetStateFlag bits)
	// Ground-truth status, derived from the actual hit-detection consumers (CHit_ResolveHitRecords@0x1CC390,
	// Player_TestStrikeInvuln_flag800@0x1D08F0). Strike i-frames are a live countdown at *(*(player+6208)+12);
	// the rest are verified PlayerStateFlag bits @player+1312. See memory sdbz-hit-damage-system.
	static void SdbzDrawStateHud(std::vector<SdbzPrim>& out, u32 player, const char* tag, float x, float y) {
		const u32 f = Ee32(player + 1312);
		auto put = [&](const char* s, ImU32 col) { EmitText(out, ImVec2(x, y), col, s); x += ImGui::CalcTextSize(s).x + 7.0f; };
		put(tag, IM_COL32(255, 255, 255, 255));
		const u32 ht = Ee32(player + 6208); // hit-timer object; +12 = strike-invuln countdown (>0 = i-frames)
		if (EeValid(ht)) {
			const s32 iv = static_cast<s32>(Ee32(ht + 12));
			if (iv > 0) { char b[16]; std::snprintf(b, sizeof(b), "INVULN:%d", iv); put(b, IM_COL32(120, 200, 255, 255)); }
		}
		if (f & 0x200u)    put("THROW-INV", IM_COL32(120, 200, 255, 255)); // PSF_THROW_INVULN
		if (f & 0x2000u)   put("GP-LO", IM_COL32(255, 200, 80, 255));      // PSF_GUARDPOINT_LOW
		if (f & 0x4000u)   put("GP-HI", IM_COL32(255, 200, 80, 255));      // PSF_GUARDPOINT_HIGH
		// NOTE: 0x20000 dropped -- it tracks P/COM control state, NOT parry (despite the enum name). The real
		// parry/counter is state-driven (State_CheckParryGuard); a verified COUNTER indicator is TODO.
	}

	// ================= SDBZ FREECAM (engine-camera override) =================
	// cam = *(0x50075C) (camera object, cached every frame by Camera_SetupRenderMatrices@0x19C7D0).
	// eye @cam+0x150 (3f, w@+0x15C=1.0); lookat @cam+0x160 (3f). The game's gluLookAt builds View@cam+0x50
	// from eye/lookat -> copied to g_ViewMatrix@0x508E90 -> g_WorldToScreen@0x508F90. So overriding eye/
	// lookat moves the rendered scene AND our overlay together. FREEZE: NOP the 6 swc1 in
	// CCameraCtrlGame_ApplyToCamera@0x2B4BC0 that write eye/lookat (else the game overwrites us each frame).
	// (= redzep CT method, EE addresses verified 1:1 in IDA.) Freecam acts during free-run (paused EE
	// doesn't re-run gluLookAt). See memory sdbz-freecam-cheatsources.
	constexpr u32 SDBZ_CAMOBJ_PTR = 0x0050075Cu;
	constexpr u32 SDBZ_FC_STORES[6] = {0x002B4E1Cu, 0x002B4E24u, 0x002B4E28u, 0x002B4E54u, 0x002B4E58u, 0x002B4E5Cu};
	// SdbzFreecam g_fc declared earlier (near g_sdbz) so JSON persistence can reach it.

	// Patch/restore the 6 eye/lookat stores. MUST run on the CPU thread (memWrite32 + recompiler clear).
	static void SdbzFcApplyFreeze(bool on) {
		for (int i = 0; i < 6; i++) {
			if (on) { g_fc.saved[i] = memRead32(SDBZ_FC_STORES[i]); memWrite32(SDBZ_FC_STORES[i], 0u); }
			else { memWrite32(SDBZ_FC_STORES[i], g_fc.saved[i]); }
		}
		if (Cpu) Cpu->Clear(0x002B4E1Cu, 0x12); // invalidate recompiled block covering all 6 stores
	}

	static void SdbzFcSetEnabled(bool on) {
		if (on == g_fc.enabled) return;
		if (on) {
			const u32 cam = Ee32(SDBZ_CAMOBJ_PTR);
			if (!EeValid(cam)) return; // no camera yet -> ignore
			// seed freecam from the live game camera so it doesn't jump
			g_fc.eye[0] = EeF32(cam + 0x150); g_fc.eye[1] = EeF32(cam + 0x154); g_fc.eye[2] = EeF32(cam + 0x158);
			const float dx = EeF32(cam + 0x160) - g_fc.eye[0], dy = EeF32(cam + 0x164) - g_fc.eye[1], dz = EeF32(cam + 0x168) - g_fc.eye[2];
			g_fc.yaw = std::atan2(dz, dx);
			g_fc.pitch = -std::atan2(dy, std::sqrt(dx * dx + dz * dz));
			g_fc.enabled = true;
			Host::RunOnCPUThread([]() { SdbzFcApplyFreeze(true); }, false);
		} else {
			g_fc.enabled = false;
			Host::RunOnCPUThread([]() { SdbzFcApplyFreeze(false); }, false);
		}
	}

	// per-frame: integrate input, write eye/lookat. Called while enabled + SDBZ running (GS thread; the
	// eye/lookat writes are plain data, harmless if they tear for a frame). Feel ported from s&box NoClip:
	// RMB mouse-look + WASD/QE move scaled by frame-time; scroll wheel sets speed (shown as a bar).
	static void SdbzFcUpdate() {
		if (!g_fc.enabled) return;
		const u32 cam = Ee32(SDBZ_CAMOBJ_PTR);
		if (!EeValid(cam)) return;
		ImGuiIO& io = ImGui::GetIO();
		const float dt60 = (io.DeltaTime > 0.0f && io.DeltaTime < 0.5f) ? io.DeltaTime * 60.0f : 1.0f;
		// scroll wheel -> move speed (log feel), unless the pointer is over our control window
		if (io.MouseWheel != 0.0f && !io.WantCaptureMouse) {
			g_fc.move_speed *= std::pow(1.15f, io.MouseWheel);
			if (g_fc.move_speed < 0.05f) g_fc.move_speed = 0.05f;
			if (g_fc.move_speed > 64.0f) g_fc.move_speed = 64.0f;
		}
#ifdef _WIN32
		auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
		// collect look input this frame (raw OS-cursor delta while RMB held, recentered each frame so it's
		// smooth and never hits a screen edge; + arrow keys), then ease it in below.
		float dyaw = 0.0f, dpitch = 0.0f;
		static POINT s_anchor{}; static bool s_looking = false;
		if (down(VK_RBUTTON)) {
			if (!s_looking) { GetCursorPos(&s_anchor); s_looking = true; }
			else {
				POINT cur; GetCursorPos(&cur);
				dyaw   += float(cur.x - s_anchor.x) * g_fc.mouse_sens;
				dpitch += float(cur.y - s_anchor.y) * g_fc.mouse_sens * (g_fc.invert_y ? -1.0f : 1.0f);
				SetCursorPos(s_anchor.x, s_anchor.y); // recenter -> pure per-frame delta
			}
		} else {
			s_looking = false;
		}
		if (down(VK_LEFT))  dyaw -= g_fc.look_speed;
		if (down(VK_RIGHT)) dyaw += g_fc.look_speed;
		if (down(VK_UP))    dpitch -= g_fc.look_speed; // up = look up (matches mouse)
		if (down(VK_DOWN))  dpitch += g_fc.look_speed;
		// frame-rate-independent ease-out: stash input, consume fraction k of the pending look each frame
		g_fc.pend_yaw += dyaw; g_fc.pend_pitch += dpitch;
		const float sm = (g_fc.look_smooth < 0.0f) ? 0.0f : (g_fc.look_smooth > 0.95f ? 0.95f : g_fc.look_smooth);
		const float k = (sm <= 0.0f) ? 1.0f : (1.0f - std::pow(sm, dt60));
		g_fc.yaw   += g_fc.pend_yaw   * k; g_fc.pend_yaw   *= (1.0f - k);
		g_fc.pitch += g_fc.pend_pitch * k; g_fc.pend_pitch *= (1.0f - k);
		if (g_fc.pitch > 1.55f)  { g_fc.pitch = 1.55f;  if (g_fc.pend_pitch > 0.0f) g_fc.pend_pitch = 0.0f; }
		if (g_fc.pitch < -1.55f) { g_fc.pitch = -1.55f; if (g_fc.pend_pitch < 0.0f) g_fc.pend_pitch = 0.0f; }
		const float cp = std::cos(g_fc.pitch), sp = std::sin(g_fc.pitch);
		const float cy = std::cos(g_fc.yaw), sy = std::sin(g_fc.yaw);
		const float fx = cy * cp, fy = -sp, fz = sy * cp; // forward unit
		const float rx = -sy, rz = cy;                    // ground-plane right unit
		float spd = g_fc.move_speed * dt60;
		if (down(VK_SHIFT)) spd *= 4.0f;
		if (down(VK_CONTROL)) spd *= 0.25f;
		float mx = 0, my = 0, mz = 0;
		if (down('W')) { mx += fx; my += fy; mz += fz; }
		if (down('S')) { mx -= fx; my -= fy; mz -= fz; }
		if (down('D')) { mx += rx; mz += rz; }
		if (down('A')) { mx -= rx; mz -= rz; }
		if (down('E')) my += 1.0f;   // world-up
		if (down('Q')) my -= 1.0f;
		g_fc.eye[0] += mx * spd; g_fc.eye[1] += my * spd; g_fc.eye[2] += mz * spd;
#else
		const float cp = std::cos(g_fc.pitch), sp = std::sin(g_fc.pitch);
		const float cy = std::cos(g_fc.yaw), sy = std::sin(g_fc.yaw);
		const float fx = cy * cp, fy = -sp, fz = sy * cp;
#endif
		EeStoreF(cam + 0x150, g_fc.eye[0]); EeStoreF(cam + 0x154, g_fc.eye[1]); EeStoreF(cam + 0x158, g_fc.eye[2]); EeStoreF(cam + 0x15C, 1.0f);
		EeStoreF(cam + 0x160, g_fc.eye[0] + fx); EeStoreF(cam + 0x164, g_fc.eye[1] + fy); EeStoreF(cam + 0x168, g_fc.eye[2] + fz); EeStoreF(cam + 0x16C, 1.0f);
		// Build + write the VIEW matrix at cam+0x50 OURSELVES so freecam survives a GLOBAL freeze. The engine's
		// gluLookAt (Mat44_BuildLookAtView@0x107E30) that normally rebuilds cam+0x50 runs INSIDE the gated gameplay
		// update; sub_2D6FB0 only COPIES cam+0x50 -> g_ViewMatrix -> g_WorldToScreen. So when the update is frozen,
		// our cam+0x50 write is the only source and it propagates to both the game's render and our overlay.
		// Engine convention (RE'd, column-major, right-handed): F=normalize(eye-lookat), S=normalize(cross(up,F)),
		// U=cross(F,S), up=(0,1,0); columns 0/1/2 = S/U/F, 4th row = -dot(S/U/F, eye). forward (fx,fy,fz) is unit,
		// so F = -forward.
		{
			const float Fx = -fx, Fy = -fy, Fz = -fz;          // F = normalize(eye - lookat) = -forward
			float Sx = Fz, Sy = 0.0f, Sz = -Fx;                // S = cross(up=(0,1,0), F) = (F.z, 0, -F.x)
			const float sl = std::sqrt(Sx * Sx + Sy * Sy + Sz * Sz);
			if (sl > 1e-4f) {                                  // skip near-vertical look (degenerate); keep last view
				Sx /= sl; Sy /= sl; Sz /= sl;
				const float Ux = Fy * Sz - Fz * Sy, Uy = Fz * Sx - Fx * Sz, Uz = Fx * Sy - Fy * Sx; // U = cross(F,S)
				const float ex = g_fc.eye[0], ey = g_fc.eye[1], ez = g_fc.eye[2];
				const float V[16] = {
					Sx, Ux, Fx, 0.0f,
					Sy, Uy, Fy, 0.0f,
					Sz, Uz, Fz, 0.0f,
					-(Sx * ex + Sy * ey + Sz * ez), -(Ux * ex + Uy * ey + Uz * ez), -(Fx * ex + Fy * ey + Fz * ez), 1.0f
				};
				for (int i = 0; i < 16; i++) EeStoreF(cam + 0x50u + 4u * static_cast<u32>(i), V[i]);
			}
		}

		// speed bar (bottom-center): logarithmic 0.05..64
		ImDrawList* dl = ImGui::GetBackgroundDrawList();
		const float W = io.DisplaySize.x, H = io.DisplaySize.y;
		const float bw = W * 0.30f, bh = 9.0f, bx = (W - bw) * 0.5f, by = H * 0.92f;
		const float t = std::log(g_fc.move_speed / 0.05f) / std::log(64.0f / 0.05f);
		dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw, by + bh), IM_COL32(0, 0, 0, 140), 3.0f);
		dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw * t, by + bh), IM_COL32(120, 200, 255, 220), 3.0f);
		dl->AddRect(ImVec2(bx, by), ImVec2(bx + bw, by + bh), IM_COL32(255, 255, 255, 170), 3.0f);
		char buf[56]; std::snprintf(buf, sizeof(buf), "FREECAM  speed %.2f  (RMB look, scroll speed, WASD/QE)", g_fc.move_speed);
		dl->AddText(ImVec2(bx, by - 16.0f), IM_COL32(255, 255, 255, 220), buf);
	}

	// ================= SDBZ FRAMESTEP (hitstop + verified code-patch NOPs) ================
	// Whole-battle freeze, every lever VERIFIED. FIGHTERS via the hitstop DATA timer (player+6224). Everything the
	// hitstop doesn't cover is frozen by the redzep-style method "find the per-frame updater instruction and NOP it"
	// -- applied as tiny code patches only while frozen (restored on unfreeze; recompiler invalidated via Cpu->Clear).
	// The g_GameFreeze@0x5D4F90 bit-0x4 "native pause" flag was a DEAD END: poking it froze NOTHING (the round timer
	// ran out with it set, confirmed live via PINE), so it's gone.
	constexpr u32 SDBZ_PLAYERS[2] = {0x005ADFB0u, 0x005AFC80u};
	constexpr u32 SDBZ_HOLD_OFF = 6224u;            // player+6224 frame-hold/hitstop timer (freezes the fighter)
	constexpr u32 SDBZ_FREEZE_HOLD = 0x40000000u;   // large positive hold, re-asserted each frame

	// Deterministic "is this slot a live fighter" check that works for ANY character. SDBZ has one player
	// CLASS PER CHARACTER (CPl01..CPl29), each with its OWN vtable -- the old ==0x4f2440 only matched CPl01,
	// so every other char (Buu, A16, ...) turned the whole overlay off. Instead we validate the actual
	// fighter structure (ground truth, no per-char hardcoding): a player-class vtable pointer into the ELF
	// image + a live posed skeleton (+1968) + a current-motion object (+1976) + a sane world-space Y (+292).
	static bool SdbzPlayerValid(u32 p) {
		if (!eeMem) return false;
		const u32 vt = Ee32(p);
		if (vt < 0x00100000u || vt >= 0x00600000u) return false; // any CPlXX vtable lives in the ELF image
		if (!EeValid(Ee32(p + 1968)) || !EeValid(Ee32(p + 1976))) return false; // skeleton + current motion
		const float py = EeF32(p + 292);
		return std::isfinite(py) && py > -200.0f && py < 600.0f;
	}

	// Battle-HUD scene-node chain (also used as the "is a real battle running" gate, so the freeze NEVER engages
	// outside a match -- e.g. CSS / menus, where setting the bit-0x2 gameplay-tick gate would hang model loading).
	constexpr u32 SDBZ_BATTLEMGR_PTR = 0x005022A8u;  // *(dword_501F7C[203]) = CGameMgr/battle mgr ptr
	constexpr u32 SDBZ_VT_CPLAYERMGR = 0x004E8160u;  // CPlayerMgr frame vtable (HUD parent)
	constexpr u32 SDBZ_VT_COMBO      = 0x004E8130u;  // CPlayerHitcht combo-container vtable
	constexpr u32 SDBZ_NODE_ALWAYSON = 56u;          // CSceneFrame node "draw-even-when-inactive" byte
	static u32 SdbzCPlayerMgr() { // returns the live CPlayerMgr node ptr, or 0 if not in a real battle
		if (!eeMem) return 0u;
		const u32 bm = Ee32(SDBZ_BATTLEMGR_PTR);
		if (!EeValid(bm)) return 0u;
		const u32 mgr = Ee32(bm + 108u);
		return (EeValid(mgr) && Ee32(mgr) == SDBZ_VT_CPLAYERMGR) ? mgr : 0u;
	}
	static bool SdbzInBattle() { return SdbzCPlayerMgr() != 0u; }

	// ===== CODE PATCHES: each entry's words overwrite EE code while frozen; restored on unfreeze. =====
	// Found by the "NOP the per-frame updater" method. NON-fighter levers verified:
	//  - ROUND TIMER: sub.s $f1,$f2 @0x3EA56C (timer -= delta, Round_FightStateUpdate) -> NOP. *** VERIFIED LIVE (PINE):
	//    NOPing it stopped the on-screen timer. Same instruction redzep's "Freeze Timer" CT NOPs (subss xmm1,xmm3).
	//  - COMBO "N hit": gate beqz $v0,0x1D4878 @0x1D47D8 (the `*(CGameMgr+96)&4` combo-advance test) -> force it
	//    UNCONDITIONAL (0x10400027 -> 0x10000027) so the per-frame pop/recompute is skipped but the number stays drawn.
	//  - HP/ENERGY BARS: per-frame HP-bar push jal HUD_SetHPBarFill_slot @0x1D03DC (+delay slot) -> NOP: bar holds its
	//    last value, still drawn.
	//  - JIGGLE/SWING BONES + projectile integrators (run AFTER the hitstop'd fighter update) -> jr $ra;nop early-return.
	//  - DETACHED SHELLS CShellBase_Update/_LateUpdate -> return 1 (alive, no work) so fired blasts freeze in place.
	static constexpr u32 P_NOP1[]  = {0x00000000u};                               // 1x NOP
	static constexpr u32 P_NOP2[]  = {0x00000000u, 0x00000000u};                  // 2x NOP (jal + delay)
	static constexpr u32 P_COMBO[] = {0x10000027u};                              // beqz $v0 -> unconditional b
	static constexpr u32 P_JRRA[]  = {0x03E00008u, 0x00000000u};                  // jr $ra ; nop
	static constexpr u32 P_RET1[]  = {0x24020001u, 0x03E00008u, 0x00000000u};     // li $v0,1 ; jr $ra ; nop
	struct SdbzPatch { u32 addr; const u32* words; u8 n; };
	static constexpr SdbzPatch SDBZ_PATCHES[] = {
		{0x003EA56Cu, P_NOP1,  1}, // round-timer sub.s -> NOP  (VERIFIED live)
		{0x001D47D8u, P_COMBO, 1}, // combo gate beqz -> b      (skip advance, "N hit" stays drawn)
		{0x001D03DCu, P_NOP2,  2}, // HP-bar per-frame push (jal+delay) -> NOP
		{0x0030FD10u, P_JRRA,  2}, // SwingBone_PreStep
		{0x003107D0u, P_JRRA,  2}, // SwingBone_SolveChain
		{0x003103A0u, P_JRRA,  2}, // swing solver (cape Lo / general)
		{0x003111B0u, P_JRRA,  2}, // swing solver (cape Hi / Mant)
		{0x00311D00u, P_JRRA,  2}, // swing solver (long-chain hair/tentacle)
		{0x00302490u, P_JRRA,  2}, // CMotionObject_IntegrateMotion (ballistic)
		{0x00302790u, P_JRRA,  2}, // CMotionObject__m16 (auto-aim ki-blasts)
		{0x003D0E20u, P_RET1,  3}, // CShellBase_Update     -> return 1
		{0x003D0EA0u, P_RET1,  3}, // CShellBase_LateUpdate -> return 1
	};
	// total words = 1+1+2 + 2*7 + 3*2 = 24
	struct SdbzFramestep { bool frozen = false; int run_frames = 0; u32 saved[24] = {0u}; bool patched = false; bool gated_req = false; };
	static SdbzFramestep g_step;

	// CPU thread: apply (or restore) all code patches. Cpu->Clear invalidates the recompiled block at each site.
	static void SdbzPhysFreeze(bool on) {
		if (on == g_step.patched) return;
		int si = 0;
		for (const SdbzPatch& p : SDBZ_PATCHES) {
			for (u8 i = 0; i < p.n; i++) {
				const u32 a = p.addr + 4u * i;
				if (on) { g_step.saved[si] = memRead32(a); memWrite32(a, p.words[i]); }
				else { memWrite32(a, g_step.saved[si]); }
				si++;
			}
			if (Cpu) Cpu->Clear(p.addr, p.n * 4u);
		}
		g_step.patched = on;
	}

	// Per-frame (GS thread): hold the fighters via hitstop; apply/lift the code patches on the idle<->advance edge
	// (so a step advances everything one frame). Camera is never touched -> freecam keeps flying while frozen.
	static void SdbzStepUpdate() {
		if (!g_step.frozen) return;
		if (!SdbzInBattle()) { // safety: only freeze in a live battle (never leak into CSS/menus)
			g_step.frozen = false; g_step.run_frames = 0;
			if (g_step.gated_req) { g_step.gated_req = false; Host::RunOnCPUThread([]() { SdbzPhysFreeze(false); }, false); }
			return;
		}
		const bool advance = g_step.run_frames > 0;
		for (u32 p : SDBZ_PLAYERS) if (SdbzPlayerValid(p)) EeStore32(p + SDBZ_HOLD_OFF, advance ? 0u : SDBZ_FREEZE_HOLD);
		const bool want_gated = !advance;
		if (want_gated != g_step.gated_req) {
			g_step.gated_req = want_gated;
			Host::RunOnCPUThread([want_gated]() { SdbzPhysFreeze(want_gated); }, false);
		}
		if (advance) g_step.run_frames--;
	}
	static void SdbzStepSetFrozen(bool on) {
		g_step.frozen = on; g_step.run_frames = 0; g_step.gated_req = on;
		if (!on) for (u32 p : SDBZ_PLAYERS) if (SdbzPlayerValid(p)) EeStore32(p + SDBZ_HOLD_OFF, 0u);
		Host::RunOnCPUThread([on]() { SdbzPhysFreeze(on); }, false);
	}
	static void SdbzStepOnce(int n = 1) { if (g_step.frozen) g_step.run_frames += n; }

	// HUD-VISIBLE-UNDER-FREEZE (deferred): the native freeze runs the node walk with advance=0, so the HUD hides
	// (only nodes with the +56 "always-on" byte draw). Writing +56 on the HUD nodes (chain battle_mgr=*(0x5022A8)
	// -> CPlayerMgr=*(bm+108) -> combo=*(+232)) crashed the earlier build, so it's removed until RE'd safely.
	// For now the freeze gives a clean no-HUD inspection view. SDBZ_VT_COMBO / SDBZ_NODE_ALWAYSON kept for later.

	// ================= SDBZ camera HORIZONTAL SCALE (the 8:7 squish) -- LIVE =================
	// The 0.75 squish is a PROJECTION PARAM stored in the camera object by sub_1C4C20: h-scale @cam+460 and its
	// reciprocal @cam+456 (a1=cam+0x10, so a1+444 / a1+440). The projection matrix is rebuilt from these params.
	// The OLD approach patched the build instruction (`lui $v0,0x3F40` @0x2B12D4) -- accurate value, but a CODE
	// patch only re-runs on a fresh camera build (camera setup), so it never updated LIVE mid-match (the boot-
	// applied widescreen pnach works only because it's in place before the camera is built). Writing the PARAM
	// directly each frame DOES update live. cam = *(0x50075C). 0.75 = default (4:3); ~0.875 = 1:1 on Native-8:7.
	// The projection MATRIX @cam+0x10 is built ONCE at camera setup (the params @cam+444 are read only by that
	// one-time builder), and RenderSetup_BuildWorldToScreen copies cam+0x10 verbatim EVERY frame. So to change
	// h-scale LIVE we overwrite the matrix X-scale element directly = cam+0x10+0 (the value the game baked with
	// 0.75). Scale it by (S/0.75) relative to the game's value, re-capturing the base if the game rebuilds it
	// (match restart). Frame-perfect (next frame's copy reflects it). cam = *(0x50075C).
	// Live camera-matrix override: h-scale (X-scale element 0 only) + zoom (X AND Y elements 0 & 5 = true FOV).
	// Both are written into the projection matrix @cam+0x10 each frame, scaled off the game's baked values.
	static void SdbzCamScaleSync() {
		static bool touched = false; static float base0 = 0.0f, base5 = 0.0f, last0 = 0.0f, last5 = 0.0f;
		if (!g_sdbz.cam_hscale_on && !touched) return; // never touched -> leave the game alone
		const u32 cam = Ee32(SDBZ_CAMOBJ_PTR);
		if (!EeValid(cam)) return;
		const float m0 = EeF32(cam + 0x10);       // projection X-scale (element 0)
		const float m5 = EeF32(cam + 0x10 + 20u); // projection Y-scale (element 5)
		if (base0 == 0.0f || std::fabs(m0 - last0) > 1e-6f) base0 = m0; // recapture if game (re)built it
		if (base5 == 0.0f || std::fabs(m5 - last5) > 1e-6f) base5 = m5;
		if (g_sdbz.cam_hscale_on) {
			const float h = (g_sdbz.cam_hscale > 0.05f) ? g_sdbz.cam_hscale : 0.05f;
			float z = g_sdbz.cam_link_zoom ? (0.75f / h) : g_sdbz.cam_zoom; // linked: widening h-scale zooms out
			if (z < 0.2f) z = 0.2f;
			last0 = base0 * (h / 0.75f) * z; // h-scale stretches X; zoom scales both axes
			last5 = base5 * z;
			EeStoreF(cam + 0x10, last0); EeStoreF(cam + 0x10 + 20u, last5);
			touched = true;
		} else {
			EeStoreF(cam + 0x10, base0); EeStoreF(cam + 0x10 + 20u, base5); last0 = base0; last5 = base5; touched = false;
		}
	}

	static void SdbzBlit(ImDrawList* dl, const std::vector<SdbzPrim>& v) {
		// Pass 1: filled triangles, globally depth-sorted far->near (painter's; ImGui has no z-buffer), drawn
		// UNDER the wireframe/text. thick carries each tri's clip-w depth.
		static std::vector<int> tris; tris.clear();
		for (int i = 0; i < static_cast<int>(v.size()); i++) if (v[i].kind == 2) tris.push_back(i);
		std::sort(tris.begin(), tris.end(), [&](int x, int y) { return v[x].thick > v[y].thick; }); // far first
		for (int i : tris) { const SdbzPrim& p = v[i]; dl->AddTriangleFilled(p.a, p.b, p.c, p.col); }
		// Pass 2: lines + text on top.
		for (const SdbzPrim& p : v) {
			if (p.kind == 0) dl->AddLine(p.a, p.b, p.col, p.thick);
			else if (p.kind == 1) dl->AddText(p.a, p.col, p.text);
		}
	}

	// The control / aesthetics window (toggle: Ctrl+J). A normal ImGui window -- PCSX2 forwards mouse
	// to ImGui unconditionally, so it is interactive whenever it is hovered (game loses the cursor then).
	// The control-panel BODY (widgets only, no Begin/End) -- shared by the in-game window and the popout window.
	static void SdbzControlBody() {
		SdbzSettings& s = g_sdbz;
		ImGui::Checkbox("Master enabled (Ctrl+H)", &s.master);
		ImGui::SameLine();
		if (ImGui::Button("Save")) SdbzSaveSettings();
		ImGui::SameLine();
		if (ImGui::Button("Reload")) SdbzLoadSettings();
		ImGui::SameLine();
		if (ImGui::Button("Defaults")) { const bool w = s.show_window, p = s.popout; s = SdbzSettings{}; s.show_window = w; s.popout = p; }
		ImGui::SameLine();
		ImGui::Checkbox("Pop out", &s.popout);

		ImGui::SeparatorText("Frame delay (free-run only)");
		ImGui::SliderInt("##delay", &s.frame_delay, 0, 8, "delay = %d frames");
		ImGui::TextDisabled("EE runs ~1f ahead of the displayed image during free-run.\nIgnored while paused/stepping (then it's already frame-perfect).");

		ImGui::SeparatorText("Box types");
		const ImGuiColorEditFlags cf = ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoInputs;
		auto row = [&](const char* lbl, bool* on, SdbzColor* col) {
			ImGui::Checkbox(lbl, on);
			ImGui::SameLine(150.0f);
			ImGui::ColorEdit4((std::string("##c") + lbl).c_str(), &col->r, cf);
		};
		row("Hurt (body)", &s.show_hurt, &s.c_hurt);
		row("Attack", &s.show_attack, &s.c_attack);
		row("Throw / grab", &s.show_throw, &s.c_throw);
		row("Proximity", &s.show_prox, &s.c_prox);
		row("Other / volume", &s.show_other, &s.c_other);
		row("Projectiles", &s.show_shells, &s.c_shell);
		row("Skeleton (Ctrl+K)", &s.show_skel, &s.c_skel);
		ImGui::Checkbox("State HUD (invuln / parry / guard)", &s.show_state);
		ImGui::Checkbox("Hide game HUD (while frozen)", &s.hide_hud);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(
			"OFF (default): the game HUD stays drawn (frozen) during the Ctrl+P freeze -- no flicker.\n"
			"ON: hides the HUD while frozen for a clean inspection view. Works by writing the HUD scene\n"
			"nodes' +56 'always-on' flag each frame (the same flag that keeps the 3D scene drawn when frozen).");

		ImGui::SeparatorText("Aspect ratio (display)");
		{
			static const char* const aspNames[] = {"Stretch (fill)", "Auto 4:3/3:2", "4:3 (PS2 hardware)", "16:9", "10:7 (true pixels)", "8:7"};
			int a = static_cast<int>(EmuConfig.CurrentAspectRatio);
			if (a < 0 || a >= IM_ARRAYSIZE(aspNames)) a = 2;
			if (ImGui::Combo("Aspect", &a, aspNames, IM_ARRAYSIZE(aspNames))) {
				EmuConfig.CurrentAspectRatio = static_cast<AspectRatioType>(a); GSConfig.AspectRatio = static_cast<AspectRatioType>(a);
			}
			// SDBZ renders a 640x448 framebuffer (= 10:7 pixel grid). Its DISPLAY register (GS_WriteCrtcRegs
			// @0x104F20) stretches that buffer to DW+1=2560 = the full 4:3 NTSC raster. So:
			//  - 4:3  = hardware-faithful: matches a real PS2/TV. 3D round (native proj h-scale 0.75), but the
			//           2D sprites/HUD/FMV (authored on the square 640x448 grid) get squished ~7% horizontally.
			//  - 10:7 = shows the raw 640x448 pixels SQUARE: 2D sprites/HUD/FMV are undistorted. The 3D then
			//           needs h-scale 0.70 (= 1/(10/7)) to stay round. Both correct at once.
			if (ImGui::Button("4:3 (PS2 hardware)")) {
				EmuConfig.CurrentAspectRatio = AspectRatioType::R4_3; GSConfig.AspectRatio = AspectRatioType::R4_3;
				s.cam_hscale_on = false; // native 0.75 projection is tuned for 4:3 output (2D ~7% squished, like real HW)
			}
			ImGui::SameLine();
			if (ImGui::Button("10:7 (2D correct)")) {
				EmuConfig.CurrentAspectRatio = AspectRatioType::R10_7; GSConfig.AspectRatio = AspectRatioType::R10_7;
				s.cam_hscale_on = true; s.cam_hscale = 0.70f; s.cam_link_zoom = false; s.cam_zoom = 1.0f; // round 3D at 10:7
			}
			ImGui::SameLine();
			if (ImGui::Button("Widescreen 16:9")) {
				EmuConfig.CurrentAspectRatio = AspectRatioType::Stretch; GSConfig.AspectRatio = AspectRatioType::Stretch;
				s.cam_hscale_on = true; s.cam_hscale = 0.46875f; s.cam_link_zoom = false; s.cam_zoom = 1.0f;
			}
			ImGui::TextDisabled("FB is 640x448 (10:7), stretched to 4:3 by the game's DISPLAY reg. '4:3' = real-HW look\n(2D ~7%% squished). '10:7' = 2D sprites/video undistorted + sets 3D h-scale 0.70 to match.");
			// Live ground truth from the GS display-env globals (GS_BuildDisplayEnv @0x105670 sets these).
			{
				u32 fbw = Ee32(0x503100), fbh = Ee32(0x503104), magh = Ee32(0x503110), vmode = Ee32(0x5030F4);
				if (fbw > 0 && fbw <= 1024 && fbh > 0 && fbh <= 1024) {
					float grid = static_cast<float>(fbw) / static_cast<float>(fbh);
					ImGui::Text("Live: FB %ux%u  grid %.3f  MAGH=%u (mag %u)  mode=%u  -> DISPLAY 4:3",
						fbw, fbh, grid, magh, magh + 1u, vmode);
				}
			}
		}

		ImGui::SeparatorText("Deinterlace");
		{
			static const char* const ilNames[] = {
				"Automatic", "Off (no deinterlace)", "Weave TFF", "Weave BFF", "Bob TFF", "Bob BFF",
				"Blend TFF", "Blend BFF", "Adaptive TFF", "Adaptive BFF" };
			int il = static_cast<int>(GSConfig.InterlaceMode);
			if (il < 0 || il >= IM_ARRAYSIZE(ilNames)) il = 0;
			if (ImGui::Combo("Mode", &il, ilNames, IM_ARRAYSIZE(ilNames))) {
				const GSInterlaceMode m = static_cast<GSInterlaceMode>(il);
				EmuConfig.GS.InterlaceMode = m; GSConfig.InterlaceMode = m; // overlay runs on the GS thread -> live
			}
			bool dio = EmuConfig.GS.DisableInterlaceOffset;
			if (ImGui::Checkbox("Disable interlace offset (kill half-line jitter)", &dio)) {
				EmuConfig.GS.DisableInterlaceOffset = dio; GSConfig.DisableInterlaceOffset = dio;
			}
			ImGui::TextDisabled("'Off' = no deinterlace, stable image for frame-perfect inspection. The game's AUTO\nno-interlacing patch needs resources/patches.zip next to the exe (now bundled).");
		}

		ImGui::SeparatorText("Camera scale (widescreen / FOV)");
		ImGui::Checkbox("Override camera (H-scale + zoom)", &s.cam_hscale_on);
		ImGui::SliderFloat("H-scale", &s.cam_hscale, 0.45f, 1.10f, "%.3f");
		ImGui::Checkbox("Link zoom to H-scale", &s.cam_link_zoom);
		if (s.cam_link_zoom) {
			float linked = 0.75f / ((s.cam_hscale > 0.05f) ? s.cam_hscale : 0.05f);
			ImGui::BeginDisabled();
			ImGui::SliderFloat("Zoom", &linked, 0.40f, 1.60f, "%.2f (linked)");
			ImGui::EndDisabled();
		} else {
			ImGui::SliderFloat("Zoom", &s.cam_zoom, 0.40f, 1.60f, "%.2f");
		}
		ImGui::TextDisabled("H-scale = 1/display-aspect for round 3D: 0.75=4:3, 0.70=10:7, 0.875=8:7, 0.469=16:9 fill.\nZoom <1 = zoom OUT/wider FOV. Linked: widening h-scale zooms out.");

		ImGui::SeparatorText("Style");
		ImGui::SliderFloat("Box thickness", &s.box_thickness, 0.5f, 4.0f, "%.1f");
		ImGui::SliderFloat("Skeleton thickness", &s.skel_thickness, 0.5f, 4.0f, "%.1f");
		ImGui::SliderInt("Sphere detail", &s.sphere_segments, 6, 40, "%d");
		ImGui::Checkbox("Shaded fill", &s.sphere_shaded);
		ImGui::SameLine();
		ImGui::SetNextItemWidth(120.0f);
		ImGui::SliderFloat("##fillA", &s.sphere_fill_alpha, 0.05f, 1.0f, "fill %.2f");

		ImGui::SeparatorText("Freecam (Ctrl+F)");
		bool fc = g_fc.enabled;
		if (ImGui::Checkbox("Enable freecam", &fc)) SdbzFcSetEnabled(fc);
		ImGui::TextDisabled("Move WASD + Q/E (up/down), look arrows.\nShift=fast, Ctrl=slow. Free-run only (not paused).\nGame camera + boxes move together.");
		ImGui::SliderFloat("Move speed", &g_fc.move_speed, 0.25f, 16.0f, "%.2f");
		ImGui::SliderFloat("Mouse sensitivity", &g_fc.mouse_sens, 0.0005f, 0.015f, "%.4f");
		ImGui::SliderFloat("Look smoothing", &g_fc.look_smooth, 0.0f, 0.95f, "%.2f");
		ImGui::SliderFloat("Look speed (arrows)", &g_fc.look_speed, 0.005f, 0.12f, "%.3f");
		ImGui::Checkbox("Invert Y", &g_fc.invert_y);
		ImGui::TextDisabled("Tune, then 'Save' (top) to lock it in across runs.");
		if (ImGui::Button("Re-seed from game cam") && g_fc.enabled) { SdbzFcSetEnabled(false); SdbzFcSetEnabled(true); }

		ImGui::SeparatorText("Framestep (Ctrl+P freeze, Ctrl+. step)");
		bool fr = g_step.frozen;
		if (ImGui::Checkbox("Freeze (fighters + bones + projectiles)", &fr)) SdbzStepSetFrozen(fr);
		ImGui::SameLine();
		if (ImGui::Button("Step")) { if (!g_step.frozen) SdbzStepSetFrozen(true); SdbzStepOnce(1); }
		ImGui::SameLine();
		if (ImGui::Button("+10")) { if (!g_step.frozen) SdbzStepSetFrozen(true); SdbzStepOnce(10); }
		ImGui::TextDisabled("Freezes fighters while the engine keeps rendering,\nso freecam can orbit a frozen pose. PCSX2 stays unpaused.");
	}

	// In-game floating control window (drawn in PCSX2's main ImGui context).
	static void SdbzControlWindow() {
		ImGui::SetNextWindowSize(ImVec2(340.0f, 0.0f), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowBgAlpha(0.85f);
		if (!ImGui::Begin("SDBZ Hitbox Overlay", &g_sdbz.show_window, ImGuiWindowFlags_NoNav)) { ImGui::End(); return; }
		SdbzControlBody();
		ImGui::End();
	}

#ifdef _WIN32
	// ================= SDBZ control-panel POPOUT (separate OS window) =================
	// Self-contained: its own ImGui context + Win32 window + a small standalone D3D11 device/swapchain,
	// decoupled from the game's (Vulkan) GS renderer. Created/rendered/pumped all on the GS thread (single-
	// threaded ImGui via SetCurrentContext), so there is no GImGui race with PCSX2's main context.
	struct SdbzPopout {
		bool active = false; HWND hwnd = nullptr;
		ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
		IDXGISwapChain* swap = nullptr; ID3D11RenderTargetView* rtv = nullptr;
		ImGuiContext* imctx = nullptr;
	};
	static SdbzPopout g_pop;

	static void SdbzPopoutMakeRTV() {
		if (g_pop.rtv) { g_pop.rtv->Release(); g_pop.rtv = nullptr; }
		ID3D11Texture2D* bb = nullptr;
		if (g_pop.swap && SUCCEEDED(g_pop.swap->GetBuffer(0, IID_PPV_ARGS(&bb))) && bb) {
			g_pop.dev->CreateRenderTargetView(bb, nullptr, &g_pop.rtv); bb->Release();
		}
	}

	static LRESULT WINAPI SdbzPopoutWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
		if (g_pop.imctx) {
			ImGuiContext* prev = ImGui::GetCurrentContext();
			ImGui::SetCurrentContext(g_pop.imctx);
			const bool handled = ImGui_ImplWin32_WndProcHandler(h, m, w, l) != 0;
			ImGui::SetCurrentContext(prev);
			if (handled) return true;
		}
		if (m == WM_SIZE && g_pop.swap && w != SIZE_MINIMIZED) {
			if (g_pop.rtv) { g_pop.rtv->Release(); g_pop.rtv = nullptr; }
			g_pop.swap->ResizeBuffers(0, static_cast<UINT>(LOWORD(l)), static_cast<UINT>(HIWORD(l)), DXGI_FORMAT_UNKNOWN, 0);
			SdbzPopoutMakeRTV();
			return 0;
		}
		if (m == WM_CLOSE) { g_sdbz.popout = false; return 0; } // X -> close (destroyed next frame)
		return DefWindowProcW(h, m, w, l);
	}

	static bool SdbzPopoutCreate() {
		const HINSTANCE hinst = GetModuleHandleW(nullptr);
		WNDCLASSEXW wc = {sizeof(wc), CS_CLASSDC, SdbzPopoutWndProc, 0, 0, hinst, nullptr,
			LoadCursorW(nullptr, IDC_ARROW), nullptr, nullptr, L"SdbzPopoutWindow", nullptr};
		RegisterClassExW(&wc);
		g_pop.hwnd = CreateWindowW(L"SdbzPopoutWindow", L"SDBZ Hitbox Overlay", WS_OVERLAPPEDWINDOW,
			120, 120, 380, 780, nullptr, nullptr, hinst, nullptr);
		if (!g_pop.hwnd) { UnregisterClassW(L"SdbzPopoutWindow", hinst); return false; }
		DXGI_SWAP_CHAIN_DESC sd = {};
		sd.BufferCount = 2; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = g_pop.hwnd;
		sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0}; D3D_FEATURE_LEVEL got;
		if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, want, 2,
				D3D11_SDK_VERSION, &sd, &g_pop.swap, &g_pop.dev, &got, &g_pop.ctx))) {
			DestroyWindow(g_pop.hwnd); g_pop.hwnd = nullptr; UnregisterClassW(L"SdbzPopoutWindow", hinst); return false;
		}
		SdbzPopoutMakeRTV();
		ImGuiContext* prev = ImGui::GetCurrentContext();
		g_pop.imctx = ImGui::CreateContext();
		ImGui::SetCurrentContext(g_pop.imctx);
		ImGui::GetIO().IniFilename = nullptr;
		ImGui::StyleColorsDark();
		ImGui_ImplWin32_Init(g_pop.hwnd);
		ImGui_ImplDX11_Init(g_pop.dev, g_pop.ctx);
		ImGui::SetCurrentContext(prev);
		ShowWindow(g_pop.hwnd, SW_SHOWNA); UpdateWindow(g_pop.hwnd);
		g_pop.active = true;
		return true;
	}

	static void SdbzPopoutDestroy() {
		if (g_pop.imctx) {
			ImGuiContext* prev = ImGui::GetCurrentContext();
			ImGui::SetCurrentContext(g_pop.imctx);
			ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown();
			ImGui::DestroyContext(g_pop.imctx);
			ImGui::SetCurrentContext(prev != g_pop.imctx ? prev : nullptr);
			g_pop.imctx = nullptr;
		}
		if (g_pop.rtv) { g_pop.rtv->Release(); g_pop.rtv = nullptr; }
		if (g_pop.swap) { g_pop.swap->Release(); g_pop.swap = nullptr; }
		if (g_pop.ctx) { g_pop.ctx->Release(); g_pop.ctx = nullptr; }
		if (g_pop.dev) { g_pop.dev->Release(); g_pop.dev = nullptr; }
		if (g_pop.hwnd) { DestroyWindow(g_pop.hwnd); g_pop.hwnd = nullptr; UnregisterClassW(L"SdbzPopoutWindow", GetModuleHandleW(nullptr)); }
		g_pop.active = false;
	}

	static void SdbzPopoutRender() { // GS thread, each frame
		if (!g_sdbz.popout) { if (g_pop.active) SdbzPopoutDestroy(); return; }
		if (!g_pop.active && !SdbzPopoutCreate()) { g_sdbz.popout = false; return; }
		MSG msg;
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
		if (!g_pop.active || !g_pop.rtv) return; // WM_CLOSE may have flipped it off
		ImGuiContext* prev = ImGui::GetCurrentContext();
		ImGui::SetCurrentContext(g_pop.imctx);
		ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
		const ImGuiViewport* vp = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(vp->WorkPos); ImGui::SetNextWindowSize(vp->WorkSize);
		if (ImGui::Begin("##sdbzpop", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
				ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus))
			SdbzControlBody();
		ImGui::End();
		ImGui::Render();
		const float clr[4] = {0.06f, 0.06f, 0.08f, 1.0f};
		g_pop.ctx->OMSetRenderTargets(1, &g_pop.rtv, nullptr);
		g_pop.ctx->ClearRenderTargetView(g_pop.rtv, clr);
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
		g_pop.swap->Present(1, 0);
		ImGui::SetCurrentContext(prev);
	}
#else
	static void SdbzPopoutRender() {}
#endif // _WIN32

	static void DrawSdbzHitboxOverlay() {
		// Load persisted settings once.
		static bool s_loaded = false;
		if (!s_loaded) { SdbzLoadSettings(); s_loaded = true; }

#ifdef _WIN32
		// Ctrl+H = master on/off; Ctrl+K = skeleton on/off; Ctrl+J = control window.
		{
			static bool s_prevH = false, s_prevK = false, s_prevJ = false, s_prevF = false, s_prevP = false, s_prevStep = false;
			const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
			const bool h = ctrl && (GetAsyncKeyState('H') & 0x8000) != 0;
			const bool k = ctrl && (GetAsyncKeyState('K') & 0x8000) != 0;
			const bool j = ctrl && (GetAsyncKeyState('J') & 0x8000) != 0;
			const bool f = ctrl && (GetAsyncKeyState('F') & 0x8000) != 0;
			const bool p = ctrl && (GetAsyncKeyState('P') & 0x8000) != 0;            // freeze (menu-less pause)
			const bool stp = ctrl && (GetAsyncKeyState(VK_OEM_PERIOD) & 0x8000) != 0; // Ctrl+.  = step 1 frame
			if (h && !s_prevH) g_sdbz.master = !g_sdbz.master;
			if (k && !s_prevK) g_sdbz.show_skel = !g_sdbz.show_skel;
			if (j && !s_prevJ) g_sdbz.show_window = !g_sdbz.show_window;
			if (f && !s_prevF) SdbzFcSetEnabled(!g_fc.enabled);
			if (p && !s_prevP) SdbzStepSetFrozen(!g_step.frozen);
			if (stp && !s_prevStep) { if (!g_step.frozen) SdbzStepSetFrozen(true); SdbzStepOnce(1); }
			s_prevH = h; s_prevK = k; s_prevJ = j; s_prevF = f; s_prevP = p; s_prevStep = stp;
		}
#endif
		// When the VM is paused, the GS thread only re-presents (and thus re-runs ImGui input) while
		// "run idle" is set. FullscreenUI uses this to stay interactive when paused; our control window
		// needs the same or it's frozen. Assert it while the window is open; on close, hand the flag
		// back to FullscreenUI's needs (don't clobber its menu). NOTE: if the window is *closed* while
		// already paused, presents have stopped, so Ctrl+J can't be polled -- open it before pausing.
		{
			static bool s_prev_window = false;
			const bool want_idle = g_sdbz.show_window || g_sdbz.popout; // keep GS thread live for either window
			if (want_idle) MTGS::SetRunIdle(true);
			else if (s_prev_window) MTGS::SetRunIdle(FullscreenUI::HasActiveWindow());
			s_prev_window = want_idle;
		}

		// Software cursor for the in-game window (usable in FULLSCREEN where PCSX2 hides the OS cursor). The
		// popout window has its own OS cursor.
		ImGui::GetIO().MouseDrawCursor = g_sdbz.show_window && !g_sdbz.popout;
		if (g_sdbz.show_window && !g_sdbz.popout) SdbzControlWindow(); // in-game window (unless popped out)
		SdbzPopoutRender(); // separate OS window when g_sdbz.popout (self-gates / no-op otherwise)

		// Freecam runs whenever SDBZ is up, independent of the box overlay's master toggle.
		const bool sdbz_running = SdbzPlayerValid(SDBZ_PLAYERS[0]) || SdbzPlayerValid(SDBZ_PLAYERS[1]);
		if (sdbz_running) {
			SdbzStepUpdate(); SdbzFcUpdate(); SdbzCamScaleSync();
		}
		else {
			// Game/battle gone (CSS, menus, between rounds): tear down so the freeze never leaks. SdbzStepSetFrozen
			// (false) releases the engine sim-freeze flag; SdbzStepUpdate's SdbzInBattle guard also catches it.
			if (g_fc.enabled) SdbzFcSetEnabled(false);
			if (g_step.frozen) SdbzStepSetFrozen(false);
		}

		if (!g_sdbz.master) return;
		if (!sdbz_running) return;
		float WS[16]; EeMat(SDBZ_WS, WS);
		for (int i = 0; i < 16; i++) if (!std::isfinite(WS[i])) return; // bad camera matrix this frame -> draw nothing

		// Build this frame's primitives, then push through a ring buffer so frame_delay can replay an
		// older frame (EE-ahead compensation). Paused/stepping => always delay 0 (RAM == displayed frame).
		static std::vector<SdbzPrim> s_ring[9];
		static int s_head = 0;
		std::vector<SdbzPrim>& cur = s_ring[s_head];
		cur.clear();

		float hudx, hudy; GSTranslateDisplayToWindowCoordinates(0.02f, 0.06f, &hudx, &hudy);
		const float hud_row = ImGui::GetFontSize() + 2.0f;
		int idx = 0;
		for (u32 player : {SDBZ_PLAYERS[0], SDBZ_PLAYERS[1]}) {
			if (SdbzPlayerValid(player)) { // any character class -- validated by structure, not vtable identity
				if (g_sdbz.show_skel) SdbzDrawSkeleton(cur, player, WS);
				SdbzDrawBoxes(cur, player, WS);
				if (g_sdbz.show_shells) SdbzDrawShells(cur, player, WS);
				if (g_sdbz.show_state) SdbzDrawStateHud(cur, player, idx == 0 ? "P1" : "P2", hudx, hudy + idx * hud_row);
			}
			idx++;
		}

		const bool paused = (VMManager::GetState() != VMState::Running);
		int d = paused ? 0 : g_sdbz.frame_delay;
		if (d < 0) d = 0; if (d > 8) d = 8;
		const int draw_idx = (s_head + 9 - d) % 9;
		SdbzBlit(ImGui::GetBackgroundDrawList(), s_ring[draw_idx]);
		s_head = (s_head + 1) % 9;
	}
} // namespace
// ====================================================================================

// Exported entry: thin external-linkage wrapper so ImGuiManager::RenderOverlays() (a different
// translation unit) can call into the overlay. The real entry DrawSdbzHitboxOverlay() above has
// internal linkage (anonymous namespace); this just forwards to it.
namespace SdbzOverlay
{
	void DrawHitboxOverlay() { DrawSdbzHitboxOverlay(); }
} // namespace SdbzOverlay
