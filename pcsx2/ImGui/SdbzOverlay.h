// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// SDBZ live hitbox overlay. Frame-perfect in-process overlay drawn at present time from live EE RAM
// (SLUS-214.42). Extracted out of ImGuiOverlays.cpp so it stays isolated from upstream churn and can
// later be driven by Lua scripts. The whole implementation lives in SdbzOverlay.cpp.
namespace SdbzOverlay
{
	// Called once per presented frame from ImGuiManager::RenderOverlays() (GS thread).
	void DrawHitboxOverlay();
} // namespace SdbzOverlay
