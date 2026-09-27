// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Sdbz/EeNative.h"
#include "Sdbz/EeHooks.h"
#include "Sdbz/RollbackDevice.h"

#include "Config.h"
#include "Memory.h"
#include "R5900.h"
#include "VU.h"

#include "common/Console.h"

#include "fmt/format.h"

#include <immintrin.h>
#include <map>

namespace EeNative
{
	namespace
	{
		// ---- EE state access ----
		// main RAM only (cached / uncached aliases); anything else declines
		inline u8* Ram(u32 addr, u32 len)
		{
			const u32 a = addr & 0x1FFFFFFFu;
			if (a >= Ps2MemSize::MainRam || a + len > Ps2MemSize::MainRam)
				return nullptr;
			return &eeMem->Main[a];
		}
		inline u32& Gpr32(int r) { return cpuRegs.GPR.r[r].UL[0]; }
		inline void SetGpr128(int r, const u8* src) { std::memcpy(&cpuRegs.GPR.r[r].UQ, src, 16); }
		inline void SetGprS32(int r, u32 v)
		{
			cpuRegs.GPR.r[r].UD[0] = static_cast<u64>(static_cast<s64>(static_cast<s32>(v)));
		}
		constexpr int A0 = 4, A1 = 5, A2 = 6, A3 = 7, A4 = 8, A5 = 9, V1 = 3;

		// VU0 macro FMAC under the default clamp config (vu0Overflow on, sign/extra off): operand clamp = min/max
		const __m128 kMax = _mm_castsi128_ps(_mm_set1_epi32(0x7f7fffff));
		const __m128 kMin = _mm_castsi128_ps(_mm_set1_epi32(static_cast<int>(0xff7fffffu)));
		inline __m128 VuClamp(__m128 v) { return _mm_max_ps(_mm_min_ps(v, kMax), kMin); }
		inline __m128 Bc(__m128 v, int i)
		{
			switch (i)
			{
				case 0: return _mm_shuffle_ps(v, v, 0x00);
				case 1: return _mm_shuffle_ps(v, v, 0x55);
				case 2: return _mm_shuffle_ps(v, v, 0xAA);
				default: return _mm_shuffle_ps(v, v, 0xFF);
			}
		}
		bool Vu0MacroConfigOk()
		{
			const auto& r = EmuConfig.Cpu.Recompiler;
			return r.vu0Overflow && !r.vu0SignOverflow && !r.vu0ExtraOverflow;
		}
		bool Vu0Idle() { return !(vuRegs[0].VI[REG_VPU_STAT].UL & 1); }

		// ---- mode gate ----
		struct Entry
		{
			u32 pc;
			std::string impl;
			Mode mode;
			u64 taken = 0, declined = 0;
		};
		std::map<u32, Entry> s_entries;
		inline bool ModeAllows(Mode m)
		{
			if (m == MODE_ALWAYS)
				return true;
			if (m == MODE_RESIM)
				return RollbackDevice::IsResimulating();
			return *RollbackDevice::ResimABFlag() != 0;
		}

		// ============ implementations ============
		// Each: (1) check preconditions, decline without side effects; (2) perform; (3) charge the EE block cycles
		// the recompiler charges for the replaced straight-line block (EECycleRate 0).

		// 0x108220 Mat44_Mul(a0 = out, a1 = A, a2 = B): VU0 macro 4x4 multiply (vf4-7 = B rows, vf8-11 = A rows)
		//   vmulax  A, vf4, vfN ; vmadday A, vf5, vfN ; vmaddaz A, vf6, vfN ; vmaddw vfN, vf7, vfN
		bool Vu0Mat44Mul(Mode mode)
		{
			if (!ModeAllows(mode) || !Vu0Idle() || !Vu0MacroConfigOk())
				return false;
			u8* out = Ram(Gpr32(A0) & ~15u, 64);
			const u8* a = Ram(Gpr32(A1) & ~15u, 64);
			const u8* b = Ram(Gpr32(A2) & ~15u, 64);
			if (!out || !a || !b)
				return false;
			VURegs& vu = vuRegs[0];
			__m128 f4 = _mm_loadu_ps(reinterpret_cast<const float*>(b + 0));
			__m128 f5 = _mm_loadu_ps(reinterpret_cast<const float*>(b + 16));
			__m128 f6 = _mm_loadu_ps(reinterpret_cast<const float*>(b + 32));
			__m128 f7 = _mm_loadu_ps(reinterpret_cast<const float*>(b + 48));
			__m128 row[4];
			for (int i = 0; i < 4; i++)
				row[i] = _mm_loadu_ps(reinterpret_cast<const float*>(a + 16 * i));
			__m128 acc = _mm_setzero_ps();
			__m128 res[4];
			for (int i = 0; i < 4; i++)
			{
				const __m128 ft = row[i];
				// MULAx (cFs): ACC = clamp(Fs) * bc.x
				acc = _mm_mul_ps(VuClamp(f4), Bc(ft, 0));
				// MADDAy (cFs): ACC = ACC + clamp(Fs) * bc.y
				acc = _mm_add_ps(acc, _mm_mul_ps(VuClamp(f5), Bc(ft, 1)));
				// MADDAz
				acc = _mm_add_ps(acc, _mm_mul_ps(VuClamp(f6), Bc(ft, 2)));
				// MADDw (COP2: cACC|cFt|cFs): Fd = clamp(Fs) * clamp(bc.w) + clamp(ACC)
				res[i] = _mm_add_ps(_mm_mul_ps(VuClamp(f7), VuClamp(Bc(ft, 3))), VuClamp(acc));
			}
			for (int i = 0; i < 4; i++)
				_mm_storeu_ps(reinterpret_cast<float*>(out + 16 * i), res[i]);
			// register state as the EE code leaves it
			_mm_storeu_ps(vu.VF[4].F, f4);
			_mm_storeu_ps(vu.VF[5].F, f5);
			_mm_storeu_ps(vu.VF[6].F, f6);
			_mm_storeu_ps(vu.VF[7].F, f7);
			for (int i = 0; i < 4; i++)
				_mm_storeu_ps(vu.VF[8 + i].F, res[i]);
			_mm_storeu_ps(vu.ACC.F, acc);
			cpuRegs.cycle += 41;
			return true;
		}

		// 0x1082A0 Mat44_Copy(a0 = dst, a1 = src): lq a2..a5 from src, sq to dst
		bool Mat44Copy(Mode mode)
		{
			if (!ModeAllows(mode))
				return false;
			u8* dst = Ram(Gpr32(A0) & ~15u, 64);
			const u8* src = Ram(Gpr32(A1) & ~15u, 64);
			if (!dst || !src)
				return false;
			alignas(16) u8 tmp[64];
			std::memcpy(tmp, src, 64);
			SetGpr128(A2, tmp);
			SetGpr128(A3, tmp + 16);
			SetGpr128(A4, tmp + 32);
			SetGpr128(A5, tmp + 48);
			std::memcpy(dst, tmp, 64);
			cpuRegs.cycle += 15;
			return true;
		}

		// 0x1082D0 Mat44_LoadIdentity(a0 = dst): v1 = &g_Mat44Identity (0x43B9D0); lqc2 vf4-7; sqc2
		bool Vu0Mat44LoadIdentity(Mode mode)
		{
			if (!ModeAllows(mode) || !Vu0Idle())
				return false;
			u8* dst = Ram(Gpr32(A0) & ~15u, 64);
			const u8* id = Ram(0x43B9D0, 64);
			if (!dst || !id)
				return false;
			VURegs& vu = vuRegs[0];
			alignas(16) u8 tmp[64];
			std::memcpy(tmp, id, 64);
			for (int i = 0; i < 4; i++)
				std::memcpy(vu.VF[4 + i].F, tmp + 16 * i, 16);
			std::memcpy(dst, tmp, 64);
			SetGprS32(V1, 0x43B9D0);
			cpuRegs.cycle += 13;
			return true;
		}

		// 0x1AD5A0 CHitBlkManager_ResetQueriedCellFlags(a0 = mgr): for each grid cell (rows +0x30, cols +0x2C, flags
		// array +0x14) with bit0 set: clear it, then for each mesh in that cell's slot list (+0x18, stride +0x0C slots,
		// up to the first NULL): +200 = (+200 & 0xFFEDF7FF) | 0x13000. The EE code scans every cell each call.
		// Cycles = exactly what the recompiler charges for the executed blocks (block starts/cycles from EeBlockProf):
		// entry 5, first row 11 (+5 per further row), first column 13 (+12 per further), not flagged 7, flagged 13 +
		// (non-empty list: 1 + 17 per mesh + 2) + 6, row end 7, exit 3 + 2. rows/cols <= 0 take unprofiled paths: decline.
		bool HitBlkResetQueriedCellFlags(Mode mode)
		{
			if (!ModeAllows(mode))
				return false;
			const u32 mgr = Gpr32(A0);
			const u8* m = Ram(mgr, 0x34);
			if (!m)
				return false;
			auto rd = [](const u8* p, u32 off) { u32 v; std::memcpy(&v, p + off, 4); return v; };
			const s32 rows = static_cast<s32>(rd(m, 0x30)), cols = static_cast<s32>(rd(m, 0x2C));
			const u32 flags = rd(m, 0x14), slots = rd(m, 0x18), stride = rd(m, 0x0C);
			if (rows <= 0 || cols <= 0 || rows > 64 || cols > 64)
				return false;
			u8* fl = Ram(flags, static_cast<u32>(rows * cols) * 4);
			if (!fl)
				return false;
			// validate every list we will walk before writing anything (decline = no side effects)
			for (s32 c = 0; c < rows * cols; c++)
			{
				u32 f;
				std::memcpy(&f, fl + 4 * c, 4);
				if (!(f & 1))
					continue;
				for (u32 k = 0;; k++)
				{
					const u8* sp = Ram(slots + 4 * (static_cast<u32>(c) * stride + k), 4);
					if (!sp || k > 4096)
						return false;
					const u32 node = rd(sp, 0);
					if (!node)
						break;
					if (!Ram(node + 200, 4))
						return false;
				}
			}
			u64 cyc = 5 + 11 + 5 * static_cast<u64>(rows - 1) + 3 + 2;
			for (s32 r = 0; r < rows; r++)
			{
				cyc += 13 + 12 * static_cast<u64>(cols - 1) + 7;
				for (s32 col = 0; col < cols; col++)
				{
					const u32 c = static_cast<u32>(col + r * cols);
					u32 f;
					std::memcpy(&f, fl + 4 * c, 4);
					if (!(f & 1))
					{
						cyc += 7;
						continue;
					}
					f &= 0xFFFFFFFEu;
					std::memcpy(fl + 4 * c, &f, 4);
					cyc += 13 + 6;
					u32 k = 0;
					for (;; k++)
					{
						const u32 node = rd(Ram(slots + 4 * (c * stride + k), 4), 0);
						if (!node)
							break;
						u8* p = Ram(node + 200, 4);
						u32 v;
						std::memcpy(&v, p, 4);
						v = (v & 0xFFEDF7FFu) | 0x13000u;
						std::memcpy(p, &v, 4);
					}
					if (k)
						cyc += 1 + 17 * static_cast<u64>(k) + 2;
				}
			}
			cpuRegs.cycle += cyc;
			return true;
		}

		// ---- registry: one static trampoline per (impl, mode) so the JIT can call it directly ----
		template <bool (*Impl)(Mode), Mode M>
		bool Tramp()
		{
			return Impl(M);
		}
		struct ImplDef
		{
			const char* name;
			EeHooks::NativeFn fn[3];
		};
#define NATIVE_IMPL(name, f) {name, {&Tramp<f, MODE_ALWAYS>, &Tramp<f, MODE_RESIM>, &Tramp<f, MODE_RESIM_AB>}}
		const ImplDef s_impls[] = {
			NATIVE_IMPL("vu0_mat44_mul", Vu0Mat44Mul),
			NATIVE_IMPL("mat44_copy", Mat44Copy),
			NATIVE_IMPL("vu0_mat44_load_identity", Vu0Mat44LoadIdentity),
			NATIVE_IMPL("sdbz_hitblk_reset_queried_cells", HitBlkResetQueriedCellFlags),
		};
#undef NATIVE_IMPL
	} // namespace

	bool Install(u32 pc, const std::string& impl, Mode mode, std::string* error)
	{
		for (const ImplDef& d : s_impls)
			if (impl == d.name)
			{
				EeHooks::AddNative(pc, d.fn[mode], EeHooks::OWNER_GAME);
				s_entries[pc] = {pc, impl, mode};
				if (mode == MODE_RESIM_AB)
					RollbackDevice::MarkABUsed();
				return true;
			}
		if (error)
			*error = fmt::format("unknown native implementation '{}'", impl);
		return false;
	}

	void RemoveAll()
	{
		for (const auto& [pc, e] : s_entries)
			EeHooks::Remove(pc);
		s_entries.clear();
	}

	std::vector<std::string> Implementations()
	{
		std::vector<std::string> out;
		for (const ImplDef& d : s_impls)
			out.push_back(d.name);
		return out;
	}

	std::string Stats()
	{
		std::string out;
		for (const auto& [pc, e] : s_entries)
			out += fmt::format("native {:08X} {} mode {}\n", pc, e.impl, static_cast<int>(e.mode));
		return out;
	}
} // namespace EeNative
