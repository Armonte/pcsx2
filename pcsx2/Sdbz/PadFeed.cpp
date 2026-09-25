// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Sdbz/PadFeed.h"

#include "common/Console.h"

#include "fmt/format.h"

#include <array>
#include <mutex>

namespace PadFeed
{
	namespace
	{
		constexpr u32 MAX_PLAYERS = 8;
		enum class Kind : u8 { Off, Const, Sequence, Mash };

		struct Feed
		{
			Kind kind = Kind::Off;
			std::vector<Step> steps;
			bool loop = false;
			size_t idx = 0;   // sequence step
			u32 left = 0;     // reads left in the current step
			u32 seed = 0;     // mash LCG state
			u16 mask = 0;
			u32 min_hold = 1, max_hold = 1;
			Step cur;
			u64 reads = 0;
		};

		std::mutex s_mtx;
		std::array<Feed, MAX_PLAYERS> s_feeds;

		u32 Lcg(u32& s)
		{
			s = s * 1103515245u + 12345u;
			return s >> 8;
		}

		// Next input for this read (EE thread, under s_mtx).
		bool NextStep(Feed& f, Step& out)
		{
			switch (f.kind)
			{
				case Kind::Off:
					return false;
				case Kind::Const:
					out = f.cur;
					return true;
				case Kind::Sequence:
					if (f.steps.empty())
						return false;
					if (f.left == 0)
					{
						if (f.idx >= f.steps.size())
						{
							if (!f.loop)
							{
								f.kind = Kind::Off; // sequence finished: hand the port back to the real pad
								return false;
							}
							f.idx = 0;
						}
						f.cur = f.steps[f.idx++];
						f.left = std::max<u32>(f.cur.frames, 1);
					}
					f.left--;
					out = f.cur;
					return true;
				case Kind::Mash:
					if (f.left == 0)
					{
						f.cur = {};
						f.cur.buttons = static_cast<u16>(Lcg(f.seed)) & f.mask;
						f.left = f.min_hold + (f.max_hold > f.min_hold ? Lcg(f.seed) % (f.max_hold - f.min_hold + 1) : 0);
					}
					f.left--;
					out = f.cur;
					return true;
			}
			return false;
		}

		const char* KindName(Kind k)
		{
			switch (k)
			{
				case Kind::Off: return "off";
				case Kind::Const: return "const";
				case Kind::Sequence: return "sequence";
				case Kind::Mash: return "mash";
			}
			return "?";
		}
	} // namespace

	void Off(u32 player)
	{
		std::lock_guard lk(s_mtx);
		if (player < MAX_PLAYERS)
			s_feeds[player] = {};
	}

	void Const(u32 player, const Step& s)
	{
		std::lock_guard lk(s_mtx);
		if (player >= MAX_PLAYERS)
			return;
		s_feeds[player] = {};
		s_feeds[player].kind = Kind::Const;
		s_feeds[player].cur = s;
	}

	void Sequence(u32 player, std::vector<Step> steps, bool loop)
	{
		std::lock_guard lk(s_mtx);
		if (player >= MAX_PLAYERS)
			return;
		s_feeds[player] = {};
		s_feeds[player].kind = Kind::Sequence;
		s_feeds[player].steps = std::move(steps);
		s_feeds[player].loop = loop;
	}

	void Mash(u32 player, u32 seed, u16 mask, u32 min_hold, u32 max_hold)
	{
		std::lock_guard lk(s_mtx);
		if (player >= MAX_PLAYERS)
			return;
		s_feeds[player] = {};
		s_feeds[player].kind = Kind::Mash;
		s_feeds[player].seed = seed;
		s_feeds[player].mask = mask;
		s_feeds[player].min_hold = std::max<u32>(min_hold, 1);
		s_feeds[player].max_hold = std::max(max_hold, s_feeds[player].min_hold);
	}

	std::string Status()
	{
		std::lock_guard lk(s_mtx);
		std::string s = "padfeed:";
		for (u32 p = 0; p < MAX_PLAYERS; p++)
		{
			const Feed& f = s_feeds[p];
			if (f.kind == Kind::Off && !f.reads)
				continue;
			s += fmt::format(" P{} {} reads {} btn {:04X}", p + 1, KindName(f.kind), f.reads, f.cur.buttons);
			if (f.kind == Kind::Sequence)
				s += fmt::format(" step {}/{}", f.idx, f.steps.size());
		}
		return s;
	}

	u32 OnPadRead(u32 player, u8* buf, u32 ret)
	{
		std::lock_guard lk(s_mtx);
		if (player >= MAX_PLAYERS || !buf)
			return ret;
		Feed& f = s_feeds[player];
		Step st;
		if (!NextStep(f, st))
			return ret;
		f.reads++;
		// DualShock2 report: [0] status 0 = ok, [1] mode (hi nibble 7 = analog, lo = payload halfwords),
		// [2..3] buttons active-low, [4..7] RX RY LX LY, [8..19] pressure (mode 0x79). Keep the pad's mode when the
		// read succeeded (digital/analog/pressure as the game configured it), else present an analog pad.
		const u8 mode = (ret && (buf[1] >> 4) == 7) ? buf[1] : 0x73;
		buf[0] = 0;
		buf[1] = mode;
		buf[2] = static_cast<u8>(~(st.buttons >> 8));
		buf[3] = static_cast<u8>(~(st.buttons & 0xFF));
		buf[4] = st.rx;
		buf[5] = st.ry;
		buf[6] = st.lx;
		buf[7] = st.ly;
		if ((mode & 0x0F) >= 9)
		{
			// pressure order: RIGHT LEFT UP DOWN TRIANGLE CIRCLE CROSS SQUARE L1 R1 L2 R2
			static constexpr u16 PRESS_BITS[12] = {0x2000, 0x8000, 0x1000, 0x4000, 0x10, 0x20, 0x40, 0x80, 0x4, 0x8, 0x1, 0x2};
			for (u32 i = 0; i < 12; i++)
				buf[8 + i] = (st.buttons & PRESS_BITS[i]) ? 0xFF : 0x00;
		}
		return 1; // one report read
	}
} // namespace PadFeed
