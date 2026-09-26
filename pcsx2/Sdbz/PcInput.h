// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <string>

// PovertyCaster input: creamybinder (3rdparty/creamybinder, the shared SDL3 controller/keyboard core every
// PovertyCaster game uses) owns local input for the whole session -- menus, offline and netplay alike -- the way
// pchost's CbInputSource does for native games. While it is active PCSX2's own SDL/XInput/DInput sources are off
// (InputManager asks OwnsDevices()), so exactly one owner opens and pumps the devices; the user's PCSX2 input
// settings are never written and come back when the session ends.
//
// Threading: everything runs on the CPU thread (the frame boundary hook); the SDL session is created lazily on the
// first Poll() so all creamybinder/SDL calls happen on that one thread.
namespace PcInput
{
	struct Params
	{
		std::string profile;      // per-game bindings: <config_dir>/creamybinder-<profile>.ini ("fuc", "sdbz", ...)
		std::string config_dir;   // default: the pcsx2-qt.exe folder (beside the game, like pchost's povertycaster.ini)
		int online_seat = -1;     // netplay: the local player's seat (0/1); -1 = offline (both seats local)
	};

	// Requests (any thread); applied on the CPU thread at the next Poll().
	void Start(const Params& params);
	void Stop();

	bool OwnsDevices(); // InputManager: PCSX2's device sources stay closed while true
	bool Active();      // a live creamybinder session exists

	// CPU thread, once per emulated frame (before the pad is read).
	void Poll();
	// Held buttons of `seat` in the PadFeed layout ((DS2 byte2 << 8) | byte3, active-high).
	u16 Buttons(int seat);

	std::string Status();
} // namespace PcInput
