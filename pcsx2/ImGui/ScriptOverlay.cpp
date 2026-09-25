// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "BuildVersion.h"
#include "Config.h"
#include "Counters.h"
#include "Memory.h" // live EE RAM via eeMem
#include "R5900.h" // script code-patch path: Cpu->Clear (recompiler invalidate for code NOP patch)
#include "GS/GS.h"
#include "GS/GSShaderCompileIndicator.h"
#include "GS/GSCapture.h"
#include "GS/GSVector.h"
#include "GS/Renderers/Common/GSDevice.h"
#ifdef _WIN32
#include "GS/Renderers/DX12/GSDevice12.h"
#include "common/RedtapeWindows.h" // GetAsyncKeyState (freecam look) + the popout Win32 window
// Generic script-GUI popout: a separate OS window with its own ImGui context + a small standalone D3D11
// device/swapchain, decoupled from the game's GS renderer (works on any backend). Renders the script's on_gui().
#include <d3d11.h>
#include "ImGui/backends/imgui_impl_win32.h"
#include "ImGui/backends/imgui_impl_dx11.h"
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
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
#include "ImGui/ScriptBridge.h"  // engine seam the script host calls into
#include "ImGui/ScriptHost.h" // scriptable, hot-reloadable overlay path
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
#include <memory>
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ImGui/ScriptOverlay.h"

// ============================== PS2 scriptable overlay engine ==============================
// Game-agnostic in-process overlay: EE-RAM access, world->screen projection, and a pixel-exact prim/draw
// pipeline, all driven by a hot-reloadable Lua script (ScriptHost) through ScriptBridge. NOTHING here knows
// it's SDBZ -- every offset / struct layout / colour / hack lives in scripts/<game>.lua. Frame-perfect because
// it draws at present time from live EE RAM on the GS thread. (SDBZ is the prototype script; SLUS-214.42.)
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
	constexpr float OVERLAY_RENDER_W = 512.0f, OVERLAY_RENDER_H = 448.0f;

	// world point -> window pixels via g_WorldToScreen (render 512x448 -> displayed image rect).
	// CRITICAL: reject NaN/Inf/out-of-range. Feeding garbage vertices to ImGui->GS corrupts the
	// render pass (black frame). pygame ignored bad coords; the GPU does not.
	static bool WorldToWindow(const float WS[16], float wx, float wy, float wz, ImVec2& out) {
		const float p[4] = {wx, wy, wz, 1.0f}; float clip[4]; RowMul(p, WS, clip);
		if (!(clip[3] > 0.0001f)) return false; // also rejects NaN (NaN > x is false)
		const float rx = clip[0] / clip[3], ry = clip[1] / clip[3];
		if (!std::isfinite(rx) || !std::isfinite(ry)) return false;
		float winx, winy;
		GSTranslateDisplayToWindowCoordinates(rx / OVERLAY_RENDER_W, ry / OVERLAY_RENDER_H, &winx, &winy);
		if (!std::isfinite(winx) || !std::isfinite(winy)) return false;
		if (winx < -20000.0f || winx > 20000.0f || winy < -20000.0f || winy > 20000.0f) return false;
		out = ImVec2(winx, winy); return true;
	}



	// One overlay primitive (already projected to screen space). on_capture builds a per-frame list on the EE
	// thread; it's handed to the GS thread in frame order (via an MTGS AsyncCall) and blitted at present time.
	struct OverlayPrim {
		int kind;       // 0 = line, 1 = text, 2 = filled triangle (a,b,c), 3 = 2D circle (a=center, thick=radius)
		ImVec2 a, b;    // line endpoints; for text a = position; for circle a = center, b.x = outline width, b.y = filled?
		ImU32 col;
		float thick;
		char text[160]; // long enough for the shell-inspector diff lines (was 28 -> truncated mid-field)
		ImVec2 c;       // 3rd vertex for filled triangles
	};
	static inline void EmitLine(std::vector<OverlayPrim>& o, ImVec2 a, ImVec2 b, ImU32 col, float th) {
		o.push_back(OverlayPrim{0, a, b, col, th, {0}});
	}
	static inline void EmitText(std::vector<OverlayPrim>& o, ImVec2 a, ImU32 col, const char* s) {
		OverlayPrim p{1, a, ImVec2(0, 0), col, 0.0f, {0}}; std::snprintf(p.text, sizeof(p.text), "%s", s); o.push_back(p);
	}
	static inline void EmitTri(std::vector<OverlayPrim>& o, ImVec2 a, ImVec2 b, ImVec2 c, ImU32 col, float depth) {
		OverlayPrim p{2, a, b, col, depth, {0}}; p.c = c; o.push_back(p); // thick field carries depth for sorting
	}
	static inline void EmitCircle(std::vector<OverlayPrim>& o, ImVec2 c, float r, ImU32 col, bool filled, float th) {
		o.push_back(OverlayPrim{3, c, ImVec2(th, filled ? 1.0f : 0.0f), col, r, {0}}); // a=center, thick=radius
	}

	// Lua draw context: the ScriptBridge draw/project helpers (bottom of this TU) read these; a null out-list makes
	// bridge draw calls no-ops. THREAD-LOCAL: on_capture runs on the EE thread (its draw.* go into the EE capture
	// list set by CaptureOnEEThread), on_frame's HUD runs on the GS thread (its draw.* go into the GS-immediate list
	// set by Draw) -- each thread has its own sink + projection matrix, so the two never clobber each other.
	static thread_local std::vector<OverlayPrim>* g_lua_out = nullptr;
	static thread_local float g_lua_ws[16] = {0};
	static bool g_lua_cursor = false; // script's "I want an interactive window" request (cursor + run-idle); GS thread
	static int  g_frame_delay = 0;    // OPTIONAL +N nudge into the presented-frame history (0 = auto-aligned); set by script
	static bool g_nav_captured = false, g_nav_original = false; // PCSX2's initial gamepad-nav flag (restored on disable)

	// EE->GS frame delivery. on_capture (EE thread, end-of-frame) projects its prim list, then ships it to the GS
	// thread as an MTGS AsyncCall enqueued JUST BEFORE that frame's VSync packet (Counters::VSyncStart). The GS
	// thread runs ring commands strictly in order, so the AsyncCall lands paired with the EXACT frame it was read
	// from -- no matter how far the EE has run ahead (loading / character select / fast-forward). The callback drops
	// the prims into g_hist (GS-thread ring) and Draw blits the newest. This replaces a count-based FIFO that could
	// accumulate a backlog across a transition and stay lagged. g_hist also lets frame_delay nudge +N frames.
	static constexpr int HIST_N = 9;
	static std::vector<OverlayPrim> g_hist[HIST_N]; // GS-thread only (written by the AsyncCall callback, read by Draw)
	static int g_hist_head = 0;

	// Force the global ImGui gamepad-nav flag (captures PCSX2's default once so the driver can restore it).
	static void ApplyGamepadNav(bool on) {
		ImGuiIO& io = ImGui::GetIO();
		if (!g_nav_captured) { g_nav_original = (io.ConfigFlags & ImGuiConfigFlags_NavEnableGamepad) != 0; g_nav_captured = true; }
		if (on) io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
		else io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
	}

	// Sphere render style -- driven by the script (ScriptBridge::SetSphereStyle); de-natives the old hardcoded sphere-style reads.
	struct SphereStyle { int segments = 20; bool shaded = true; float fill_alpha = 0.5f; float thickness = 2.0f; };
	static SphereStyle g_sphere_style;

	// Proper UV sphere (latitude/longitude mesh), replacing the old 3-great-circle look. Optional translucent
	// SHADED FILL: lit by a fixed light, painter's-sorted back-to-front (correct for a convex sphere). W =
	// local->world matrix (a bone matrix, or identity for world-space shells); center/r are in that frame.
	static void OverlaySphere(std::vector<OverlayPrim>& out, const float WS[16], const float W[16],
	                       float cx, float cy, float cz, float r, ImU32 col) {
		constexpr int MAXST = 22, MAXSL = 42;
		int ST = g_sphere_style.segments / 2; if (ST < 3) ST = 3; if (ST > MAXST - 1) ST = MAXST - 1; // latitude
		int SL = g_sphere_style.segments;     if (SL < 6) SL = 6; if (SL > MAXSL - 1) SL = MAXSL - 1; // longitude
		static ImVec2 scr[MAXST][MAXSL]; static float dep[MAXST][MAXSL]; static bool ok[MAXST][MAXSL];
		static float nrm[MAXST][MAXSL][3]; // GS thread only, fully rewritten before read each call
		// Engine-faithful placement (RE'd 1:1 from CHitData_GridInsert@0x1ADDD0, the authoritative collision
		// geometry): the box CENTER is transformed by the FULL bone matrix W=node+240 (incl. any per-move bone
		// SCALE -- e.g. Majin Buu stretch moves), but the RADIUS (hitobj+204) is used RAW in WORLD units; the
		// engine NEVER scales r by the bone matrix. So transform the center ONCE here, then offset the sphere by
		// r in world space. (The old code transformed (center + r*n) by W, which inflated r by the bone scale and
		// drew a box far bigger than the real hit -- the "looks like it hits Vegeta but whiffs" bug.)
		const float c4[4] = {cx, cy, cz, 1.0f}; float wc[4]; RowMul(c4, W, wc); // world center (bone-scaled, like the engine)
		for (int i = 0; i <= ST; i++) {
			const float th = 3.14159265f * static_cast<float>(i) / static_cast<float>(ST);
			const float ct = std::cos(th), stt = std::sin(th);
			for (int j = 0; j <= SL; j++) {
				const float ph = 6.28318531f * static_cast<float>(j) / static_cast<float>(SL);
				const float lnx = stt * std::cos(ph), lny = ct, lnz = stt * std::sin(ph); // unit normal (world; sphere is orientation-free)
				const float wp[4] = {wc[0] + r * lnx, wc[1] + r * lny, wc[2] + r * lnz, 1.0f}; // world surface point: center + r in WORLD units
				ImVec2 sv; ok[i][j] = WorldToWindow(WS, wp[0], wp[1], wp[2], sv); scr[i][j] = sv;
				nrm[i][j][0] = lnx; nrm[i][j][1] = lny; nrm[i][j][2] = lnz; // world normal = surface direction (radius unscaled)
				dep[i][j] = wp[0] * WS[3] + wp[1] * WS[7] + wp[2] * WS[11] + WS[15]; // clip w = depth
			}
		}
		if (g_sphere_style.shaded) {
			const float lx = 0.50f, ly = 0.70f, lz = 0.51f; // fixed light direction
			const int aI = static_cast<int>(static_cast<float>((col >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f * g_sphere_style.fill_alpha * 255.0f);
			const int br = (col >> IM_COL32_R_SHIFT) & 0xFF, bg = (col >> IM_COL32_G_SHIFT) & 0xFF, bb = (col >> IM_COL32_B_SHIFT) & 0xFF;
			// Emit each quad's 2 tris with their depth (clip w). OverlayBlit globally depth-sorts ALL fill tris
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
		const float wth = g_sphere_style.thickness;
		for (int i = 1; i < ST; i++) for (int j = 0; j < SL; j++)
			if (ok[i][j] && ok[i][j + 1]) EmitLine(out, scr[i][j], scr[i][j + 1], col, wth); // latitude rings
		for (int j = 0; j < SL; j++) for (int i = 0; i < ST; i++)
			if (ok[i][j] && ok[i + 1][j]) EmitLine(out, scr[i][j], scr[i + 1][j], col, wth); // longitude lines
	}


	static void OverlayBlit(ImDrawList* dl, const std::vector<OverlayPrim>& v) {
		// Pass 1: filled triangles, globally depth-sorted far->near (painter's; ImGui has no z-buffer), drawn
		// UNDER the wireframe/text. thick carries each tri's clip-w depth.
		static std::vector<int> tris; tris.clear();
		for (int i = 0; i < static_cast<int>(v.size()); i++) if (v[i].kind == 2) tris.push_back(i);
		std::sort(tris.begin(), tris.end(), [&](int x, int y) { return v[x].thick > v[y].thick; }); // far first
		for (int i : tris) { const OverlayPrim& p = v[i]; dl->AddTriangleFilled(p.a, p.b, p.c, p.col); }
		// Pass 2: lines + text on top. Text gets a dark padded background box + 1px shadow so it stays readable
		// over any scene (raw colored text on a bright/busy frame is unreadable -- the inspector lines especially).
		for (const OverlayPrim& p : v) {
			if (p.kind == 0) { dl->AddLine(p.a, p.b, p.col, p.thick); continue; }
			if (p.kind == 3) { // 2D circle: b.y = filled flag, b.x = outline width, thick = radius
				if (p.b.y > 0.5f) dl->AddCircleFilled(p.a, p.thick, p.col, 32);
				else dl->AddCircle(p.a, p.thick, p.col, 32, p.b.x);
				continue;
			}
			if (p.kind != 1) continue;
			const ImVec2 ts = ImGui::CalcTextSize(p.text);
			dl->AddRectFilled(ImVec2(p.a.x - 3.0f, p.a.y - 1.0f), ImVec2(p.a.x + ts.x + 3.0f, p.a.y + ts.y + 1.0f),
			                  IM_COL32(0, 0, 0, 190), 2.0f);
			dl->AddText(ImVec2(p.a.x + 1.0f, p.a.y + 1.0f), IM_COL32(0, 0, 0, 230), p.text); // shadow
			dl->AddText(p.a, p.col, p.text);
		}
	}


	// ===================== SCRIPT-GUI POPOUT (generic, Windows) =====================
	// A separate OS window with its OWN ImGui context + standalone D3D11 swapchain, decoupled from the game's GS
	// renderer. Each frame (on the GS thread) it runs the script's on_gui() into itself, so the control panel can
	// live off the game image / on another monitor. Knows NOTHING about SDBZ -- it just renders Script::RunGui().
#ifdef _WIN32
	struct ScriptPopout {
		bool active = false; HWND hwnd = nullptr;
		ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
		IDXGISwapChain* swap = nullptr; ID3D11RenderTargetView* rtv = nullptr;
		ImGuiContext* imctx = nullptr;
		bool dragging = false; int drag_dx = 0, drag_dy = 0; // manual, non-blocking title-bar drag (see PopoutWndProc)
	};
	static ScriptPopout g_pop;

	static void PopoutMakeRTV() {
		if (g_pop.rtv) { g_pop.rtv->Release(); g_pop.rtv = nullptr; }
		ID3D11Texture2D* bb = nullptr;
		if (g_pop.swap && SUCCEEDED(g_pop.swap->GetBuffer(0, IID_PPV_ARGS(&bb))) && bb) {
			g_pop.dev->CreateRenderTargetView(bb, nullptr, &g_pop.rtv); bb->Release();
		}
	}

	static LRESULT WINAPI PopoutWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
		// Clicking the popout must give it keyboard FOCUS, or text entry (Ctrl+click a slider to type) never reaches
		// it: it's shown SW_SHOWNA (unfocused so popping out doesn't steal focus from the game) and ImGui consumes
		// the click without DefWindowProc activating the window. Force foreground/focus on any mouse-button-down.
		if (m == WM_LBUTTONDOWN || m == WM_RBUTTONDOWN || m == WM_MBUTTONDOWN) {
			if (GetForegroundWindow() != h) { SetForegroundWindow(h); SetFocus(h); }
		}
		// Manual, NON-BLOCKING title-bar drag. DefWindowProc's HTCAPTION drag spins a modal move loop on THIS thread
		// (the GS thread, which pumps the popout) -> the GS thread stops presenting and the whole emulation freezes
		// while you move the window. Track the drag ourselves + reposition with SetWindowPos so the GS thread keeps
		// presenting the game. (Handled before ImGui: a title-bar drag never touches the panel content.)
		switch (m) {
			case WM_NCLBUTTONDOWN:
				if (w == HTCAPTION) {
					POINT cur; GetCursorPos(&cur);
					RECT wr; GetWindowRect(h, &wr);
					g_pop.dragging = true; g_pop.drag_dx = cur.x - wr.left; g_pop.drag_dy = cur.y - wr.top;
					SetCapture(h);
					return 0;
				}
				break;
			case WM_MOUSEMOVE:
				if (g_pop.dragging) {
					POINT cur; GetCursorPos(&cur);
					SetWindowPos(h, nullptr, cur.x - g_pop.drag_dx, cur.y - g_pop.drag_dy, 0, 0,
						SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
					return 0;
				}
				break;
			case WM_LBUTTONUP:
				if (g_pop.dragging) { g_pop.dragging = false; ReleaseCapture(); return 0; }
				break;
		}
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
			PopoutMakeRTV();
			return 0;
		}
		if (m == WM_CLOSE) { Script::SetPopout(false); return 0; } // X -> dock back in-game (destroyed next frame)
		return DefWindowProcW(h, m, w, l);
	}

	static bool PopoutCreate() {
		// Capture PCSX2's live ImGui style NOW (main context is current) so the popout is 1:1 -- same theme colours
		// + scale fields (FontScaleMain/Dpi). We also load PCSX2's own Roboto TTF below so the font matches too.
		const ImGuiStyle mainStyle = ImGui::GetStyle();
		const HINSTANCE hinst = GetModuleHandleW(nullptr);
		WNDCLASSEXW wc = {sizeof(wc), CS_CLASSDC, PopoutWndProc, 0, 0, hinst, nullptr,
			LoadCursorW(nullptr, IDC_ARROW), nullptr, nullptr, L"ScriptPopoutWindow", nullptr};
		RegisterClassExW(&wc);
		g_pop.hwnd = CreateWindowW(L"ScriptPopoutWindow", L"Script Overlay", WS_OVERLAPPEDWINDOW,
			120, 120, 480, 720, nullptr, nullptr, hinst, nullptr);
		if (!g_pop.hwnd) { UnregisterClassW(L"ScriptPopoutWindow", hinst); return false; }
		DXGI_SWAP_CHAIN_DESC sd = {};
		sd.BufferCount = 2; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = g_pop.hwnd;
		sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0}; D3D_FEATURE_LEVEL got;
		if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, want, 2,
				D3D11_SDK_VERSION, &sd, &g_pop.swap, &g_pop.dev, &got, &g_pop.ctx))) {
			DestroyWindow(g_pop.hwnd); g_pop.hwnd = nullptr; UnregisterClassW(L"ScriptPopoutWindow", hinst); return false;
		}
		PopoutMakeRTV();
		ImGuiContext* prev = ImGui::GetCurrentContext();
		g_pop.imctx = ImGui::CreateContext();
		ImGui::SetCurrentContext(g_pop.imctx);
		ImGui::GetIO().IniFilename = nullptr;
		ImGui::GetStyle() = mainStyle; // 1:1 PCSX2 theme: colours + scaled metrics + FontScaleMain/Dpi
		// Load PCSX2's own Roboto at its base size (FONT_BASE_SIZE=15); the copied FontScaleMain/Dpi scales it the
		// same as in-game. Panel is text-only, so we skip PCSX2's icon/emoji merges. Fall back if the file is gone.
		ImFontConfig fcfg;
		const std::string fontpath = EmuFolders::GetOverridableResourcePath("fonts" FS_OSPATH_SEPARATOR_STR "Roboto-Regular.ttf");
		if (!ImGui::GetIO().Fonts->AddFontFromFileTTF(fontpath.c_str(), 15.0f, &fcfg))
			ImGui::GetIO().Fonts->AddFontDefault();
		ImGui_ImplWin32_Init(g_pop.hwnd);
		ImGui_ImplDX11_Init(g_pop.dev, g_pop.ctx);
		ImGui::SetCurrentContext(prev);
		ShowWindow(g_pop.hwnd, SW_SHOWNA); UpdateWindow(g_pop.hwnd);
		g_pop.active = true;
		return true;
	}

	static void PopoutDestroy() {
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
		if (g_pop.hwnd) { DestroyWindow(g_pop.hwnd); g_pop.hwnd = nullptr; UnregisterClassW(L"ScriptPopoutWindow", GetModuleHandleW(nullptr)); }
		g_pop.active = false;
	}

	static void PopoutRender() { // GS thread, each frame
		if (!Script::WantsPopout()) { if (g_pop.active) PopoutDestroy(); return; }
		if (!g_pop.active && !PopoutCreate()) { Script::SetPopout(false); return; }
		MSG msg;
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
		if (!g_pop.active || !g_pop.rtv) return; // WM_CLOSE may have flipped it off
		ImGuiContext* prev = ImGui::GetCurrentContext();
		ImGui::SetCurrentContext(g_pop.imctx);
		ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
		// Snap the script's FIRST window to fill the popout each frame (so the panel fills the OS window instead of
		// floating inside it, and can't be dragged away). Extra windows the script opens still float.
		const ImGuiViewport* vp = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(vp->WorkPos);
		ImGui::SetNextWindowSize(vp->WorkSize);
		Script::RunGui(); // the script draws its own imgui window(s) into this popout context
		ImGui::Render();
		const float clr[4] = {0.06f, 0.06f, 0.08f, 1.0f};
		g_pop.ctx->OMSetRenderTargets(1, &g_pop.rtv, nullptr);
		g_pop.ctx->ClearRenderTargetView(g_pop.rtv, clr);
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
		g_pop.swap->Present(1, 0);
		ImGui::SetCurrentContext(prev);
	}
#else
	static void PopoutRender() {}
#endif // _WIN32

	// Generic per-frame entry: drive the Lua script. ALL game knowledge (offsets, drawing, UI, freeze, freecam,
	// camera, training) now lives in the script; this just hands it a draw context and keeps the GS thread
	// interactive. Game-agnostic -- a second game is a different scripts/<game>.lua against this same engine.
	static void DrawScriptOverlay() {
		const bool enabled = Script::IsEnabled();

		// Keep the GS thread presenting + show a software cursor while the script wants an interactive window (it
		// requests this via engine.set_cursor). Without run-idle, presents stop when the VM is paused and the
		// script's window + framestep would stall. g_lua_cursor is last frame's request (1-frame lag is harmless).
		{
			static bool s_prev = false;
			const bool want = enabled && g_lua_cursor;
			if (want) MTGS::SetRunIdle(true);
			else if (s_prev) MTGS::SetRunIdle(FullscreenUI::HasActiveWindow());
			s_prev = want;
			// Do NOT force ImGui's software cursor: the main window uses the Qt backend, which manages the OS
			// cursor itself (shown whenever windowed / paused -- i.e. whenever the overlay window is actually being
			// used). A forced software cursor on top of that OS cursor = the "double cursor" bug. Rely on the OS one.
			ImGui::GetIO().MouseDrawCursor = false;
		}
		// Popout window: renders the script's on_gui() into its own OS window when popped out, and tears the window
		// down otherwise (incl. when the overlay is disabled). Self-gating + own imgui context -> safe to call here.
		PopoutRender();

		if (!enabled) {
			g_lua_cursor = false;
			if (g_nav_captured) ApplyGamepadNav(g_nav_original); // overlay off -> hand gamepad nav back to PCSX2
			for (auto& h : g_hist) h.clear(); // drop stale captures so re-enabling starts clean
			return;
		}

		// GS-thread script work: on_frame (engine bookkeeping + the stats HUD) + the in-game on_gui. The HUD draws
		// into a GS-immediate prim list that's blitted right away (stat text isn't position-critical). The
		// frame-perfect box GEOMETRY was captured on the EE thread and ring-delivered into g_hist (see below).
		static std::vector<OverlayPrim> s_gs_imm;
		s_gs_imm.clear();
		g_lua_out = &s_gs_imm;       // thread_local: the GS thread's sink
		Script::RunFrame();
		g_lua_out = nullptr;

		// The EE-captured geometry for THIS frame was already dropped into g_hist by the AsyncCall the GS thread ran
		// in ring order just before this present, so g_hist_head-1 is exactly this frame. frame_delay nudges +N.
		int d = g_frame_delay; if (d < 0) d = 0; if (d > HIST_N - 1) d = HIST_N - 1;
		const int idx = (g_hist_head - 1 - d + HIST_N * 2) % HIST_N;
		ImDrawList* dl = ImGui::GetBackgroundDrawList();
		OverlayBlit(dl, g_hist[idx]); // frame-perfect geometry (EE-captured, ring-delivered)
		OverlayBlit(dl, s_gs_imm);    // GS-immediate HUD / bookkeeping draws
	}
} // namespace
// ====================================================================================

// ScriptBridge: external-linkage seam the Lua host (ScriptHost.cpp) calls into. Thin wrappers over the
// anonymous-namespace engine primitives above, so sol2's heavy headers stay isolated in that TU. The
// draw/project helpers use the per-frame context (g_lua_out / g_lua_ws) that DrawScriptOverlay sets
// around Script::RunFrame(); they no-op while g_lua_out is null. Game knowledge stays in the .lua.
namespace ScriptBridge
{
	uint32_t Read32(uint32_t a) { return Ee32(a); }
	uint16_t Read16(uint32_t a) { return Ee16(a); }
	uint8_t Read8(uint32_t a) { return eeMem ? eeMem->Main[a & 0x01FFFFFFu] : static_cast<u8>(0); }
	float ReadF32(uint32_t a) { return EeF32(a); }
	void WriteData32(uint32_t a, uint32_t v) { EeStore32(a, v); }
	void ReadMatrix(uint32_t a, float out[16]) { EeMat(a, out); }
	bool MemReady() { return eeMem != nullptr; }

	void ApplyMatrix(const float M[16], float x, float y, float z, float out[3]) {
		const float p[4] = {x, y, z, 1.0f}; float o[4]; RowMul(p, M, o);
		out[0] = o[0]; out[1] = o[1]; out[2] = o[2];
	}
	bool WorldToScreen(float wx, float wy, float wz, float& sx, float& sy) {
		ImVec2 v; if (!WorldToWindow(g_lua_ws, wx, wy, wz, v)) return false;
		sx = v.x; sy = v.y; return true;
	}
	bool DisplayToScreen(float u, float v, float& sx, float& sy) {
		GSTranslateDisplayToWindowCoordinates(u, v, &sx, &sy);
		return std::isfinite(sx) && std::isfinite(sy);
	}

	void DrawSphere(const float W[16], float cx, float cy, float cz, float r, uint32_t rgba) {
		if (g_lua_out) OverlaySphere(*g_lua_out, g_lua_ws, W, cx, cy, cz, r, static_cast<ImU32>(rgba));
	}
	void DrawSphereWorld(float cx, float cy, float cz, float r, uint32_t rgba) {
		if (!g_lua_out) return;
		static const float ident[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
		OverlaySphere(*g_lua_out, g_lua_ws, ident, cx, cy, cz, r, static_cast<ImU32>(rgba));
	}
	void DrawLineWorld(float x1, float y1, float z1, float x2, float y2, float z2, uint32_t rgba, float th) {
		if (!g_lua_out) return;
		ImVec2 a, b;
		if (WorldToWindow(g_lua_ws, x1, y1, z1, a) && WorldToWindow(g_lua_ws, x2, y2, z2, b))
			EmitLine(*g_lua_out, a, b, static_cast<ImU32>(rgba), th);
	}
	// Native per-mesh face draw -- the whole loop (matrix read once, per-vertex transform/project, per-edge emit)
	// runs here in C++ on LIVE EE memory, so the script makes ONE call per collision mesh instead of thousands of
	// apply_matrix_at/line_world crossings. No caching: re-reads every call, so destructible/moving collision stays
	// accurate. See ScriptBridge.h for the record/pool layout.
	void DrawMeshFaces(uint32_t matAddr, uint32_t poolAddr, uint32_t recsAddr, int faceCount, int recStride,
		int vcountOff, int startOff, int normalOff, uint32_t floorRgba, uint32_t wallRgba, float ceilThresh, float th) {
		if (!g_lua_out || faceCount < 1 || faceCount > 200000) return;
		if (floorRgba == 0 && wallRgba == 0) return;
		float M[16]; EeMat(matAddr, M); // local->world, sampled once for the whole mesh
		for (int f = 0; f < faceCount; f++) {
			const u32 rec = recsAddr + static_cast<u32>(recStride) * static_cast<u32>(f);
			const u32 nv = Ee32(rec + static_cast<u32>(vcountOff));
			if (nv < 2 || nv > 64) continue;
			const float ny = EeF32(rec + static_cast<u32>(normalOff));
			const ImU32 col = static_cast<ImU32>((ny >= ceilThresh || ny <= -ceilThresh) ? floorRgba : wallRgba);
			if (col == 0) continue; // that category toggled off
			const u32 start = Ee32(rec + static_cast<u32>(startOff));
			ImVec2 first{}, prev{}; bool okFirst = false, okPrev = false;
			for (u32 k = 0; k < nv; k++) {
				const u32 v = poolAddr + 16u * (start + k);
				const float lp[4] = {EeF32(v), EeF32(v + 4), EeF32(v + 8), 1.0f};
				float w[4]; RowMul(lp, M, w);
				ImVec2 s; const bool ok = WorldToWindow(g_lua_ws, w[0], w[1], w[2], s);
				if (k == 0) { first = s; okFirst = ok; }
				if (okPrev && ok) EmitLine(*g_lua_out, prev, s, col, th); // edge prev->cur (both visible)
				prev = s; okPrev = ok;
			}
			if (nv > 2 && okPrev && okFirst) EmitLine(*g_lua_out, prev, first, col, th); // close the polygon
		}
	}
	// Native walk+draw of a whole collision subtree (the SDBZ stage). DFS the scene tree, and for every collision
	// node (mesh-head whose owner backref points home) read its world matrix ONCE and draw all its CHitMesh faces.
	// Per mesh: a vertex-projection cache (each shared vertex projected once) + shared-edge DEDUP (each edge drawn
	// once, not once per incident face). Live EE reads every frame -> destructible geometry stays correct.
	void DrawCollisionTree(const CollTreeDraw& p) {
		if (!g_lua_out || !eeMem) return;
		auto inRam = [](u32 a) { return a >= 0x100000u && a < 0x2000000u; };
		if (!inRam(p.root) || (p.floorRgba == 0 && p.wallRgba == 0 && p.obstRgba == 0)) return;
		static const float identM[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
		static thread_local std::vector<u32> stack;
		static thread_local std::unordered_set<u32> seen;
		static thread_local std::unordered_map<u32, std::pair<ImVec2, u8>> vcache; // pool index -> (screen, valid)
		static thread_local std::unordered_set<u64> edges;                          // drawn-edge set (pool index pair)
		auto isHidden = [&](u32 node) -> bool {
			if (!p.hidden || !p.hidden[0]) return false;
			char nm[16]; int i = 0;
			for (; i < 15; i++) { const u8 c = eeMem->Main[(node + p.nameOff + i) & 0x01FFFFFFu]; nm[i] = (char)c; if (!c) break; }
			const size_t nlen = static_cast<size_t>(i);
			if (!nlen) return false;
			for (const char* s = p.hidden; *s; ) {
				const char* e = s; while (*e && *e != ',') e++;
				if (static_cast<size_t>(e - s) == nlen && std::memcmp(s, nm, nlen) == 0) return true;
				s = (*e == ',') ? e + 1 : e;
			}
			return false;
		};
		stack.clear(); seen.clear();
		stack.push_back(p.root);
		int budget = 20000;
		while (!stack.empty() && budget-- > 0) {
			const u32 node = stack.back(); stack.pop_back();
			if (!inRam(node) || !seen.insert(node).second) continue;
			const u32 head = Ee32(node + p.headOff);
			if (inRam(head) && Ee32(head + p.ownerOff) == node && !isHidden(node)) { // a (visible) collision node
				float M[16]; EeMat(node + p.matOff, M);
				const bool brk = (p.brkGrid != 0 && Ee32(node + p.gridOff) == p.brkGrid); // breakable -> obstacle colour
				for (u32 mesh = head; inRam(mesh); mesh = Ee32(mesh + p.nextOff)) {
					const u32 count = Ee32(mesh + p.countOff);
					const u32 recs = Ee32(mesh + p.recsOff);
					const u32 pool = Ee32(mesh + p.poolOff);
					if (count < 1 || count > 8192 || !inRam(recs) || !inRam(pool)) { // no face data -> sphere (obstacle)
						const float r = p.obstRgba ? EeF32(mesh + p.radiusOff) : 0.0f;
						if (r > 0.0f) {
							const float lc[4] = {EeF32(mesh + p.centerOff), EeF32(mesh + p.centerOff + 4), EeF32(mesh + p.centerOff + 8), 1.0f};
							float wc[4]; RowMul(lc, M, wc);
							OverlaySphere(*g_lua_out, g_lua_ws, identM, wc[0], wc[1], wc[2], r, p.obstRgba);
						}
						continue;
					}
					vcache.clear(); edges.clear(); // per-mesh (the vertex pool is per-mesh, so indices don't collide)
					auto getv = [&](u32 idx, ImVec2& out) -> bool { // project vertex `idx` once, cache it
						const auto it = vcache.find(idx);
						if (it != vcache.end()) { out = it->second.first; return it->second.second != 0; }
						const u32 v = pool + 16u * idx;
						const float lp[4] = {EeF32(v), EeF32(v + 4), EeF32(v + 8), 1.0f};
						float w[4]; RowMul(lp, M, w);
						ImVec2 s; const bool ok = WorldToWindow(g_lua_ws, w[0], w[1], w[2], s);
						vcache.emplace(idx, std::make_pair(s, static_cast<u8>(ok ? 1 : 0)));
						out = s; return ok;
					};
					for (u32 f = 0; f < count; f++) {
						const u32 rec = recs + static_cast<u32>(p.recStride) * f;
						const u32 nv = Ee32(rec + p.vcountOff);
						if (nv < 2 || nv > 64) continue;
						const float ny = EeF32(rec + p.normalOff);
						const ImU32 col = brk ? static_cast<ImU32>(p.obstRgba)
							: static_cast<ImU32>((ny >= p.ceilThresh || ny <= -p.ceilThresh) ? p.floorRgba : p.wallRgba);
						if (col == 0) continue;
						const u32 start = Ee32(rec + p.startOff);
						for (u32 k = 0; k < nv; k++) {
							const u32 ia = start + k, ib = start + ((k + 1) % nv);
							const u32 lo = ia < ib ? ia : ib, hi = ia < ib ? ib : ia;
							if (!edges.insert((static_cast<u64>(lo) << 32) | hi).second) continue; // edge already drawn
							ImVec2 a, b; const bool oka = getv(ia, a); const bool okb = getv(ib, b);
							if (oka && okb) EmitLine(*g_lua_out, a, b, col, p.thick);
						}
					}
				}
			}
			const u32 child = Ee32(node + p.childOff); if (inRam(child)) stack.push_back(child);
			const u32 sib = Ee32(node + p.sibOff); if (inRam(sib)) stack.push_back(sib);
		}
	}
	void DrawTextScreen(float sx, float sy, uint32_t rgba, const char* s) {
		if (g_lua_out && s) EmitText(*g_lua_out, ImVec2(sx, sy), static_cast<ImU32>(rgba), s);
	}
	void DrawLineScreen(float x0, float y0, float x1, float y1, uint32_t rgba, float th) {
		if (g_lua_out) EmitLine(*g_lua_out, ImVec2(x0, y0), ImVec2(x1, y1), static_cast<ImU32>(rgba), th);
	}
	void DrawCircleScreen(float cx, float cy, float r, uint32_t rgba, bool filled, float th) {
		if (g_lua_out) EmitCircle(*g_lua_out, ImVec2(cx, cy), r, static_cast<ImU32>(rgba), filled, th);
	}
	// 2D filled triangle (e.g. nav-cube faces). Reuses the kind-2 prim, so it shares the global far->near fill sort;
	// pass a small `depth` (relative to 3D clip-w, which is large) so HUD fills land on top of scene fills, and vary
	// it per face for correct back-to-front layering within the widget.
	void DrawTriScreen(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t rgba, float depth) {
		if (g_lua_out) EmitTri(*g_lua_out, ImVec2(x0, y0), ImVec2(x1, y1), ImVec2(x2, y2), static_cast<ImU32>(rgba), depth);
	}

	// Tell the engine which world->screen matrix to project with this frame. The ADDRESS is game-specific, so the
	// SCRIPT chooses it (keeping engine code game-agnostic). Reads 16 contiguous floats; on a non-finite matrix
	// (e.g. no camera yet) zero it so projected draws no-op cleanly instead of corrupting the render pass.
	void SetWorldMatrix(uint32_t addr) {
		float m[16]; EeMat(addr, m);
		bool ok = true;
		for (int i = 0; i < 16; i++) if (!std::isfinite(m[i])) { ok = false; break; }
		for (int i = 0; i < 16; i++) g_lua_ws[i] = ok ? m[i] : 0.0f;
	}
	// The script reports whether it currently wants an interactive window (drives the software cursor + run-idle).
	void SetCursorVisible(bool on) { g_lua_cursor = on; }
	// Draw delay (0..8 frames): the engine replays an older frame's prim list to line the overlay up with the
	// displayed frame (EE RAM leads the screen). ALWAYS honoured -- including while the emulator is paused / input-
	// recording, which is exactly when frame-accurate alignment matters. It's the user's setting; no engine override.
	void SetFrameDelay(int frames) { g_frame_delay = frames < 0 ? 0 : (frames > 8 ? 8 : frames); }
	void SetGamepadNav(bool on) { ApplyGamepadNav(on); }

	void WriteDataF32(uint32_t a, float v) { EeStoreF(a, v); }
	void WriteData8(uint32_t a, uint8_t v) { EeStore8(a, v); }

	// recompiler-safe EE code-patch registry (script-managed freeze/freecam NOPs). Fixed slots avoid a heap map;
	// ALL access is on the CPU thread (inside RunOnCPUThread), so no locking is needed.
	struct LuaPatch { u32 addr; u32 orig; bool used; };
	static LuaPatch s_lua_patches[64] = {};
	void PatchCode(uint32_t addr, uint32_t word) {
		Host::RunOnCPUThread([addr, word]() {
			int freeslot = -1;
			for (int i = 0; i < 64; i++) {
				if (s_lua_patches[i].used && s_lua_patches[i].addr == addr) { memWrite32(addr, word); if (Cpu) Cpu->Clear(addr, 4); return; }
				if (freeslot < 0 && !s_lua_patches[i].used) freeslot = i;
			}
			if (freeslot < 0) return; // registry full
			s_lua_patches[freeslot] = {addr, memRead32(addr), true};
			memWrite32(addr, word);
			if (Cpu) Cpu->Clear(addr, 4);
		}, false);
	}
	void UnpatchCode(uint32_t addr) {
		Host::RunOnCPUThread([addr]() {
			for (int i = 0; i < 64; i++) {
				if (s_lua_patches[i].used && s_lua_patches[i].addr == addr) {
					memWrite32(addr, s_lua_patches[i].orig); s_lua_patches[i].used = false; if (Cpu) Cpu->Clear(addr, 4); return;
				}
			}
		}, false);
	}

	// Restore EVERY active script patch. The registry is a static that outlives the sol::state, so a code NOP
	// applied by one script instance (e.g. the freeze round-timer NOP @0x3EA56C) would otherwise survive a reload
	// / disable and silently keep the timer frozen with freeze showing OFF. The host calls this on script teardown;
	// a fresh script re-asserts its cfg-driven patches (hide-HUD / no-cull / deint) on its next on_frame, while
	// runtime-only patches (freeze / freecam) correctly stay off until re-toggled.
	static bool g_mouse_claimed = false; // script claims the cursor (gizmo) this frame -> DisplayWidget skips dbl-click fullscreen
	void UnpatchAll() {
		g_mouse_claimed = false; // clear any stale claim on script reload/disable
		Host::RunOnCPUThread([]() {
			for (int i = 0; i < 64; i++) {
				if (s_lua_patches[i].used) {
					memWrite32(s_lua_patches[i].addr, s_lua_patches[i].orig);
					if (Cpu) Cpu->Clear(s_lua_patches[i].addr, 4);
					s_lua_patches[i].used = false;
				}
			}
		}, false);
	}

	bool KeyDown(int vk) {
#ifdef _WIN32
		return (GetAsyncKeyState(vk) & 0x8000) != 0;
#else
		(void)vk; return false;
#endif
	}
	void MouseDelta(float& dx, float& dy) { const ImGuiIO& io = ImGui::GetIO(); dx = io.MouseDelta.x; dy = io.MouseDelta.y; }
	void MousePos(float& x, float& y) { const ImGuiIO& io = ImGui::GetIO(); x = io.MousePos.x; y = io.MousePos.y; }
	bool MouseDown(int b) { const ImGuiIO& io = ImGui::GetIO(); return (b >= 0 && b < 5) && io.MouseDown[b]; }
	float MouseWheel() { return ImGui::GetIO().MouseWheel; }
	bool WantMouse() { return ImGui::GetIO().WantCaptureMouse; }
	void SetMouseClaimed(bool on) { g_mouse_claimed = on; }
	bool MouseClaimed() { return g_mouse_claimed; }
	// Freecam mouse-look, 1:1 with the native: while RMB is held, return the RAW OS cursor delta and re-center the
	// cursor to its anchor each frame -> pure per-frame delta with no screen-edge clamp (infinite look). Returns
	// (0,0) when RMB is up. Windows-only (raw cursor); other platforms fall back to no look.
	void LookDelta(float& dx, float& dy) {
		dx = 0.0f; dy = 0.0f;
#ifdef _WIN32
		static POINT s_anchor{}; static bool s_looking = false;
		if ((GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0) {
			if (!s_looking) { GetCursorPos(&s_anchor); s_looking = true; }
			else {
				POINT cur; GetCursorPos(&cur);
				dx = static_cast<float>(cur.x - s_anchor.x); dy = static_cast<float>(cur.y - s_anchor.y);
				SetCursorPos(s_anchor.x, s_anchor.y); // recenter -> edge-free infinite look
			}
		} else {
			s_looking = false;
		}
#endif
	}

	void SetSphereStyle(int segments, bool shaded, float fill_alpha, float thickness) {
		g_sphere_style.segments = segments; g_sphere_style.shaded = shaded;
		g_sphere_style.fill_alpha = fill_alpha; g_sphere_style.thickness = thickness;
	}
	void SetAspect(int idx) {
		const AspectRatioType a = static_cast<AspectRatioType>(idx);
		EmuConfig.CurrentAspectRatio = a; GSConfig.AspectRatio = a;
	}
	int GetAspect() { return static_cast<int>(EmuConfig.CurrentAspectRatio); }
	void SetDeinterlace(int mode) {
		const GSInterlaceMode m = static_cast<GSInterlaceMode>(mode);
		EmuConfig.GS.InterlaceMode = m; GSConfig.InterlaceMode = m;
	}
	int GetDeinterlace() { return static_cast<int>(GSConfig.InterlaceMode); }
	void SetInterlaceOffset(bool on) { EmuConfig.GS.DisableInterlaceOffset = on; GSConfig.DisableInterlaceOffset = on; }
	bool GetInterlaceOffset() { return GSConfig.DisableInterlaceOffset; }
} // namespace ScriptBridge

// Exported entry: thin external-linkage wrapper so ImGuiManager::RenderOverlays() (a different
// translation unit) can call into the overlay. The real entry DrawScriptOverlay() above has
// internal linkage (anonymous namespace); this just forwards to it.
namespace ScriptOverlay
{
	void Draw() { DrawScriptOverlay(); }

	// EE/CPU thread (Counters::VSyncStart): run the script's geometry pass against the now-settled frame and queue
	// its screen-space prims for the GS thread. g_lua_out is thread_local, so this EE-thread sink is independent of
	// the GS-thread HUD sink. WorldToWindow's only GS dependency (GSTranslate -> s_last_draw_rect) is a benign read.
	void CaptureOnEEThread() {
		if (!Script::IsEnabled())
			return;
		auto cap = std::make_shared<std::vector<OverlayPrim>>();
		static thread_local size_t s_capHint = 4096;
		cap->reserve(s_capHint); // size to ~last frame so the thousands of collision-line push_backs don't realloc
		g_lua_out = cap.get();
		Script::RunCapture(); // on_capture() -> draw.* -> screen prims into *cap
		g_lua_out = nullptr;
		const size_t capN = cap->size(); s_capHint = capN + capN / 4 + 256; // next-frame hint (size + 25% + margin)
		// Ship to the GS thread as an AsyncCall. We're in Counters::VSyncStart, just BEFORE gsPostVsyncStart()
		// enqueues this frame's VSync, so the GS thread runs this callback in ring order right before it presents
		// THIS frame -> the prims are paired with the exact frame they were read from, with no count-based drift.
		// (shared_ptr because std::function must be copyable; the vector itself is only moved, never copied.)
		MTGS::RunOnGSThread([cap]() {
			g_hist[g_hist_head] = std::move(*cap);
			g_hist_head = (g_hist_head + 1) % HIST_N;
		});
	}
} // namespace ScriptOverlay
