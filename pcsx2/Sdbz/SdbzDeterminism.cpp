// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Sdbz/SdbzDeterminism.h"
#include "Sdbz/SdbzSavestate.h"

#include "Memory.h" // eeMem + memRead32/memWrite32
#include "R5900.h"  // Cpu->Clear: recompiler invalidate after the fetch patch
#include "Config.h" // EmuFolders
#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

namespace
{
	// ---- SLUS-21442 knowledge (overridable from Lua; see SDBZ_ROLLBACK_PLAN.md 6.) ----
	constexpr u32 DEF_DATA_START = 0x00440000;      // below the first known mutable global; rodata overlap is harmless
	constexpr u32 ADDR_HEAP_HIWATER = 0x00500708;   // g_HeapHighWaterEnd -- the game's own malloc high-water mark
	constexpr u32 HEAP_END_FALLBACK = 0x00C00000;   // if the high-water read looks bogus
	constexpr u32 HEAP_END_CAP = 0x01F00000;        // never snapshot past this (leave the stack top alone)
	constexpr u32 ADDR_PAD_SLOTS = 0x0056A260;      // g_PadSlots[4] -> heap-allocated 92-byte pad objects
	constexpr u32 PAD_OBJ_SIZE = 92;
	constexpr u32 ADDR_PAD_FETCH_JAL = 0x00198DAC;  // jal Pad_FetchRawState inside Pad_UpdateOne_ReadAndParse
	constexpr u32 INSN_LI_V0_1 = 0x24020001;        // li $v0,1 -> "fetch succeeded", raw pad bytes stay ours

	__fi const u8* EePtr(u32 addr) { return &eeMem->Main[addr & 0x01FFFFFFu]; }
	__fi u8* EePtrW(u32 addr) { return &eeMem->Main[addr & 0x01FFFFFFu]; }
	__fi u32 EeRead32(u32 addr) { return *reinterpret_cast<const u32*>(EePtr(addr)); }

	__fi u64 Fnv1a64(const u8* p, size_t n, u64 h = 0xCBF29CE484222325ULL)
	{
		for (size_t i = 0; i < n; i++)
			h = (h ^ p[i]) * 0x100000001B3ULL;
		return h;
	}

	enum class Mode : int
	{
		Idle,
		Recording,
		Replaying,
	};

	struct PadFrame
	{
		u8 pads[4][PAD_OBJ_SIZE];
		u8 present[4];
	};

	struct State
	{
		// config
		u32 dataStart = DEF_DATA_START;
		u32 heapEndCfg = 0; // 0 = live high-water
		u32 chunkBytes = 64 * 1024;
		u32 maxFrames = 3600;
		std::vector<SdbzSavestate::PreserveBlock> excludes;

		// runtime
		std::unique_ptr<SdbzSavestate> baseline;
		std::vector<PadFrame> padLog;
		std::vector<std::vector<u64>> chunkLog; // [frame][chunk]
		u32 frameIdx = 0;
		u32 firstDivergentFrame = UINT32_MAX;
		u32 divergentFrames = 0;
		std::vector<u32> chunkDivergeCount; // per-chunk histogram across the replay
		bool fetchPatched = false;
	};

	State s_st;
	std::mutex s_mtx; // guards s_st between control calls (any thread) and OnVSyncStart (EE thread)
	std::atomic<int> s_mode{static_cast<int>(Mode::Idle)};
	std::atomic<int> s_pendingCmd{0}; // 0 none, 1 baseline, 2 replay, 3 stop

	std::vector<SdbzSavestate::PreserveBlock> DefaultExcludes()
	{
		return {
			{0x00500798, 0x40}, // g_FileLoadMngState: async CD-load state machine -- must never roll back
			{0x004FB120, 0x10}, // file-load mgr RFS cursor
			{0x00500700, 0x10}, // alloc stats + g_HeapHighWaterEnd (self-referential; keep linear)
		};
	}

	u32 ResolveHeapEnd(const State& st)
	{
		if (st.heapEndCfg)
			return std::min(st.heapEndCfg, HEAP_END_CAP);
		const u32 hiwater = EeRead32(ADDR_HEAP_HIWATER);
		if (hiwater <= st.dataStart || hiwater > HEAP_END_CAP)
		{
			Console.Warning("SdbzDeterminism: heap high-water 0x%08X looks bogus, using fallback 0x%08X", hiwater,
				HEAP_END_FALLBACK);
			return HEAP_END_FALLBACK;
		}
		return (hiwater + 0xFFFu) & ~0xFFFu; // page-align up
	}

	void RecordPads(PadFrame& pf)
	{
		for (u32 i = 0; i < 4; i++)
		{
			const u32 padPtr = EeRead32(ADDR_PAD_SLOTS + i * 4);
			pf.present[i] = (padPtr >= 0x100000u && padPtr < 0x2000000u) ? 1 : 0;
			if (pf.present[i])
				std::memcpy(pf.pads[i], EePtr(padPtr), PAD_OBJ_SIZE);
			else
				std::memset(pf.pads[i], 0, PAD_OBJ_SIZE);
		}
	}

	void WritePads(const PadFrame& pf)
	{
		for (u32 i = 0; i < 4; i++)
		{
			if (!pf.present[i])
				continue;
			const u32 padPtr = EeRead32(ADDR_PAD_SLOTS + i * 4);
			if (padPtr < 0x100000u || padPtr >= 0x2000000u)
				continue;
			// Write ONLY the pre-parse inputs: status+raw buttons+raw analog [0,30) and the
			// config flags [64,68). The game's parse then derives cur/prev/pressed/repeat from
			// the pad object's own (restored/evolved) history -- writing the full post-parse
			// block would double-shift the edge-detection state and desync the replay.
			std::memcpy(EePtrW(padPtr), pf.pads[i], 30);
			std::memcpy(EePtrW(padPtr + 64), &pf.pads[i][64], 4);
		}
	}

	void ChecksumChunks(const SdbzSavestate& ss, u32 chunkBytes, std::vector<u64>& out)
	{
		out.clear();
		for (const SdbzSavestate::BackupLoc& loc : ss.Regions())
		{
			for (u32 a = loc.startAddress; a < loc.endAddress; a += chunkBytes)
			{
				const u32 n = std::min(chunkBytes, loc.endAddress - a);
				out.push_back(Fnv1a64(EePtr(a), n));
			}
		}
	}

	// map a flat chunk index back to its EE address range (for reporting)
	bool ChunkToRange(const SdbzSavestate& ss, u32 chunkBytes, u32 chunkIdx, u32& startOut, u32& endOut)
	{
		u32 idx = 0;
		for (const SdbzSavestate::BackupLoc& loc : ss.Regions())
		{
			for (u32 a = loc.startAddress; a < loc.endAddress; a += chunkBytes)
			{
				if (idx == chunkIdx)
				{
					startOut = a;
					endOut = std::min(a + chunkBytes, loc.endAddress);
					return true;
				}
				idx++;
			}
		}
		return false;
	}

	// Direct patch (NOT via ScriptBridge::PatchCode -- that queues through Host::RunOnCPUThread and
	// could land a frame late; we're already ON the CPU thread here, and the patch must be active
	// before the next frame's Pad_UpdateAll).
	u32 s_fetchOrigWord = 0;

	void PatchFetchOff()
	{
		if (!s_st.fetchPatched)
		{
			s_fetchOrigWord = memRead32(ADDR_PAD_FETCH_JAL);
			memWrite32(ADDR_PAD_FETCH_JAL, INSN_LI_V0_1);
			if (Cpu)
				Cpu->Clear(ADDR_PAD_FETCH_JAL, 4);
			s_st.fetchPatched = true;
		}
	}

	void UnpatchFetch()
	{
		if (s_st.fetchPatched)
		{
			memWrite32(ADDR_PAD_FETCH_JAL, s_fetchOrigWord);
			if (Cpu)
				Cpu->Clear(ADDR_PAD_FETCH_JAL, 4);
			s_st.fetchPatched = false;
		}
	}

	void WriteReport()
	{
		const std::string path = Path::Combine(EmuFolders::DataRoot, "sdbz_determinism_report.txt");
		auto fp = FileSystem::OpenManagedCFile(path.c_str(), "w");
		if (!fp)
			return;
		std::fprintf(fp.get(), "SDBZ determinism replay report\n");
		std::fprintf(fp.get(), "frames compared: %u / %zu recorded\n", s_st.frameIdx, s_st.chunkLog.size());
		std::fprintf(fp.get(), "divergent frames: %u (first: %u)\n", s_st.divergentFrames,
			s_st.firstDivergentFrame == UINT32_MAX ? 0 : s_st.firstDivergentFrame);
		std::fprintf(fp.get(), "\ndivergent chunks (histogram over the whole replay):\n");
		for (u32 c = 0; c < s_st.chunkDivergeCount.size(); c++)
		{
			if (!s_st.chunkDivergeCount[c])
				continue;
			u32 a = 0, b = 0;
			if (s_st.baseline && ChunkToRange(*s_st.baseline, s_st.chunkBytes, c, a, b))
				std::fprintf(fp.get(), "  [0x%08X - 0x%08X): %u frame(s)\n", a, b, s_st.chunkDivergeCount[c]);
		}
		Console.WriteLn("SdbzDeterminism: report written to %s", path.c_str());
	}
} // namespace

namespace SdbzDeterminism
{
	void CaptureBaseline() { s_pendingCmd.store(1); }
	void StartReplay() { s_pendingCmd.store(2); }
	void Stop() { s_pendingCmd.store(3); }

	std::string Status()
	{
		char buf[192];
		const Mode m = static_cast<Mode>(s_mode.load());
		const char* mn = (m == Mode::Idle) ? "idle" : (m == Mode::Recording) ? "recording" : "replaying";
		std::lock_guard<std::mutex> lk(s_mtx);
		std::snprintf(buf, sizeof(buf), "%s | frame %u/%zu | divergent %u (first %d) | state %u KB", mn, s_st.frameIdx,
			s_st.padLog.size(), s_st.divergentFrames,
			(s_st.firstDivergentFrame == UINT32_MAX) ? -1 : static_cast<int>(s_st.firstDivergentFrame),
			s_st.baseline ? s_st.baseline->TotalBytes() / 1024u : 0u);
		return buf;
	}

	void SetRegion(u32 dataStart, u32 heapEnd)
	{
		std::lock_guard<std::mutex> lk(s_mtx);
		s_st.dataStart = dataStart;
		s_st.heapEndCfg = heapEnd;
	}

	void AddExclude(u32 addr, u32 len)
	{
		std::lock_guard<std::mutex> lk(s_mtx);
		s_st.excludes.push_back({addr, len});
	}

	void ClearExcludes()
	{
		std::lock_guard<std::mutex> lk(s_mtx);
		s_st.excludes = DefaultExcludes();
	}

	void SetChunkKB(u32 kb)
	{
		std::lock_guard<std::mutex> lk(s_mtx);
		s_st.chunkBytes = std::max(1u, kb) * 1024u;
	}

	void SetMaxFrames(u32 frames)
	{
		std::lock_guard<std::mutex> lk(s_mtx);
		s_st.maxFrames = std::max(60u, frames);
	}

	void OnVSyncStart()
	{
		if (!eeMem)
			return;

		std::lock_guard<std::mutex> lk(s_mtx);

		// ---- queued commands (issued from the GS/Lua thread) ----
		switch (s_pendingCmd.exchange(0))
		{
			case 1: // baseline: snapshot + start recording
			{
				if (s_st.excludes.empty())
					s_st.excludes = DefaultExcludes();
				const u32 heapEnd = ResolveHeapEnd(s_st);
				s_st.baseline = std::make_unique<SdbzSavestate>(s_st.dataStart, heapEnd, s_st.excludes);
				s_st.baseline->Capture();
				s_st.padLog.clear();
				s_st.chunkLog.clear();
				s_st.frameIdx = 0;
				s_st.firstDivergentFrame = UINT32_MAX;
				s_st.divergentFrames = 0;
				s_st.chunkDivergeCount.clear();
				UnpatchFetch();
				s_mode.store(static_cast<int>(Mode::Recording));
				Console.WriteLn("SdbzDeterminism: baseline captured (0x%08X-0x%08X, %u KB), recording...",
					s_st.dataStart, heapEnd, s_st.baseline->TotalBytes() / 1024u);
				break;
			}
			case 2: // replay: restore + patch fetch + compare
			{
				if (!s_st.baseline || s_st.padLog.empty())
				{
					Console.Error("SdbzDeterminism: no baseline/recording to replay");
					break;
				}
				s_st.baseline->Load({});
				PatchFetchOff();
				s_st.frameIdx = 0;
				s_st.firstDivergentFrame = UINT32_MAX;
				s_st.divergentFrames = 0;
				s_st.chunkDivergeCount.assign(s_st.chunkLog.empty() ? 0 : s_st.chunkLog[0].size(), 0);
				s_mode.store(static_cast<int>(Mode::Replaying));
				Console.WriteLn("SdbzDeterminism: state restored, replaying %zu frame(s)...", s_st.padLog.size());
				break;
			}
			case 3:
				UnpatchFetch();
				s_mode.store(static_cast<int>(Mode::Idle));
				Console.WriteLn("SdbzDeterminism: stopped");
				break;
			default:
				break;
		}

		// ---- per-frame work ----
		const Mode m = static_cast<Mode>(s_mode.load());
		if (m == Mode::Recording)
		{
			if (s_st.frameIdx >= s_st.maxFrames)
			{
				s_mode.store(static_cast<int>(Mode::Idle));
				Console.WriteLn("SdbzDeterminism: recording complete (%u frames). rollback.replay() when ready.",
					s_st.frameIdx);
				return;
			}
			PadFrame pf;
			RecordPads(pf);
			s_st.padLog.push_back(pf);
			s_st.chunkLog.emplace_back();
			ChecksumChunks(*s_st.baseline, s_st.chunkBytes, s_st.chunkLog.back());
			s_st.frameIdx++;
		}
		else if (m == Mode::Replaying)
		{
			// Compare THIS frame's settled state against the recording (frame k was recorded at
			// the same boundary), then feed the pads for the NEXT frame.
			if (s_st.frameIdx > 0 && s_st.frameIdx <= s_st.chunkLog.size())
			{
				static std::vector<u64> cur;
				ChecksumChunks(*s_st.baseline, s_st.chunkBytes, cur);
				const std::vector<u64>& rec = s_st.chunkLog[s_st.frameIdx - 1];
				bool diverged = false;
				for (size_t c = 0; c < cur.size() && c < rec.size(); c++)
				{
					if (cur[c] != rec[c])
					{
						diverged = true;
						if (c < s_st.chunkDivergeCount.size())
							s_st.chunkDivergeCount[c]++;
						if (s_st.divergentFrames < 4) // keep the console readable
						{
							u32 a = 0, b = 0;
							ChunkToRange(*s_st.baseline, s_st.chunkBytes, static_cast<u32>(c), a, b);
							Console.Error("SdbzDeterminism: frame %u diverges in [0x%08X-0x%08X)", s_st.frameIdx - 1,
								a, b);
						}
					}
				}
				if (diverged)
				{
					s_st.divergentFrames++;
					if (s_st.firstDivergentFrame == UINT32_MAX)
						s_st.firstDivergentFrame = s_st.frameIdx - 1;
				}
			}

			if (s_st.frameIdx < s_st.padLog.size())
			{
				WritePads(s_st.padLog[s_st.frameIdx]);
				s_st.frameIdx++;
			}
			else
			{
				UnpatchFetch();
				s_mode.store(static_cast<int>(Mode::Idle));
				Console.WriteLn("SdbzDeterminism: replay done. %u/%zu divergent frame(s), first=%d", s_st.divergentFrames,
					s_st.padLog.size(),
					(s_st.firstDivergentFrame == UINT32_MAX) ? -1 : static_cast<int>(s_st.firstDivergentFrame));
				WriteReport();
			}
		}
	}
} // namespace SdbzDeterminism
