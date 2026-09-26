// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Sdbz/PcInput.h"

#include "Host.h"
#include "VMManager.h"
#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"

#include "fmt/format.h"

#include <creamybinder/creamybinder.hpp>

#include <atomic>
#include <memory>
#include <mutex>

namespace PcInput
{
	namespace
	{
		// The DualShock2 as a neutral creamybinder action set (the binder shows these names; a game profile can bind
		// any device input to any of them). Order = action index.
		enum Action : int
		{
			A_UP, A_DOWN, A_LEFT, A_RIGHT, A_CROSS, A_CIRCLE, A_SQUARE, A_TRIANGLE,
			A_L1, A_R1, A_L2, A_R2, A_START, A_SELECT, A_L3, A_R3, A_COUNT
		};
		const char* const ACTION_NAMES[A_COUNT] = {"Up", "Down", "Left", "Right", "Cross", "Circle", "Square", "Triangle",
			"L1", "R1", "L2", "R2", "Start", "Select", "L3", "R3"};
		const bool ACTION_IS_DIR[A_COUNT] = {true, true, true, true};
		// PadFeed layout ((byte2 << 8) | byte3, active-high): byte2 = SELECT L3 R3 START UP RIGHT DOWN LEFT (bit 0..7),
		// byte3 = L2 R2 L1 R1 TRIANGLE CIRCLE CROSS SQUARE (bit 0..7)
		const u16 ACTION_PAD_BIT[A_COUNT] = {0x1000, 0x4000, 0x8000, 0x2000, 0x0040, 0x0020, 0x0080, 0x0010,
			0x0004, 0x0008, 0x0001, 0x0002, 0x0800, 0x0100, 0x0200, 0x0400};

		std::mutex s_req_mtx;
		bool s_req_start = false, s_req_stop = false;
		Params s_req_params;

		std::atomic<bool> s_owns{false};
		std::unique_ptr<cb::Session> s_session;
		Params s_params;
		u16 s_buttons[2] = {};
		std::string s_status = "pcinput: off";

		void ReloadPcsx2Sources()
		{
			// InputManager re-evaluates every source through IsInputSourceEnabled, which asks OwnsDevices()
			VMManager::ReloadInputSources();
		}

		void DoStart(const Params& p)
		{
			s_params = p;
			if (s_params.config_dir.empty())
				s_params.config_dir = Path::GetDirectory(FileSystem::GetProgramPath());
			if (s_params.profile.empty())
				s_params.profile = "ps2";
			// close PCSX2's device sources first: one owner per device, and SDL's subsystem refcount drops to ours
			s_owns.store(true, std::memory_order_release);
			ReloadPcsx2Sources();

			cb::ActionDescriptor desc;
			desc.count = A_COUNT;
			desc.names = ACTION_NAMES;
			desc.isDirection = ACTION_IS_DIR;
			desc.players = 2;
			desc.up = A_UP;
			desc.down = A_DOWN;
			desc.left = A_LEFT;
			desc.right = A_RIGHT;
			cb::Config cfg;
			cfg.configDir = s_params.config_dir;
			cfg.profile = s_params.profile;
			cfg.players = 2;
			cfg.socd = cb::Socd::LRNeutralUDUp; // pchost's fighting-game default: L+R neutral, U+D -> Up
			cfg.deadzone = 0.30f;
			cfg.keyboardBackend = cb::KeyboardBackend::Win32;
			cfg.focusGate = true;
			s_session = cb::Session::create(desc, cfg);
			if (!s_session)
			{
				Console.Error("PcInput: creamybinder Session::create failed: PCSX2 input restored");
				s_owns.store(false, std::memory_order_release);
				ReloadPcsx2Sources();
				s_status = "pcinput: session create failed";
				return;
			}
			if (s_params.online_seat >= 0)
				s_session->setOnlineSeat(s_params.online_seat);
			s_session->poll(); // enumerate devices before the first frame reads them
			Console.WriteLn("PcInput: creamybinder owns input (profile %s, config %s, %d device(s)%s)", s_params.profile.c_str(),
				s_params.config_dir.c_str(), s_session->deviceCount(),
				s_params.online_seat >= 0 ? fmt::format(", online seat {}", s_params.online_seat).c_str() : "");
		}

		void DoStop()
		{
			if (!s_owns.load(std::memory_order_acquire))
				return;
			s_session.reset(); // releases the devices and creamybinder's SDL subsystem references
			s_buttons[0] = s_buttons[1] = 0;
			s_owns.store(false, std::memory_order_release);
			ReloadPcsx2Sources(); // PCSX2's own sources from the user's settings again
			s_status = "pcinput: off";
			Console.WriteLn("PcInput: creamybinder released input: PCSX2 input sources restored");
		}
	} // namespace

	void Start(const Params& params)
	{
		std::lock_guard lk(s_req_mtx);
		s_req_params = params;
		s_req_start = true;
		s_req_stop = false;
	}
	void Stop()
	{
		std::lock_guard lk(s_req_mtx);
		s_req_stop = true;
		s_req_start = false;
	}
	bool OwnsDevices() { return s_owns.load(std::memory_order_acquire); }
	bool Active() { return s_session != nullptr; }

	void Poll()
	{
		{
			std::lock_guard lk(s_req_mtx);
			if (s_req_stop)
			{
				s_req_stop = false;
				DoStop();
			}
			if (s_req_start)
			{
				s_req_start = false;
				DoStop();
				DoStart(s_req_params);
			}
		}
		if (!s_session)
			return;
		s_session->poll(); // SDL pump + gamepads + Win32 keyboard, SOCD, focus gate
		for (int seat = 0; seat < 2; seat++)
		{
			const cb::ActionMask held = s_session->read(seat).held;
			u16 b = 0;
			for (int a = 0; a < A_COUNT; a++)
				if (held & cb::actionBit(a))
					b |= ACTION_PAD_BIT[a];
			s_buttons[seat] = b;
		}
		s_status = fmt::format("pcinput: {} device(s), P1 {:04X} P2 {:04X}", s_session->deviceCount(), s_buttons[0], s_buttons[1]);
	}

	u16 Buttons(int seat) { return (seat >= 0 && seat < 2) ? s_buttons[seat] : 0; }
	std::string Status() { return s_status; }
} // namespace PcInput
