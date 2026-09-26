// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <string>
#include <vector>

// Incremental (page copy-on-write) rollback snapshot ring for in-game (Slippi-style) rollback.
//
// Slippi's SlippiSavestate memcpys every region on every capture (~7.5 MiB for Melee). This ring
// keeps the same contract -- game memory only, captured/restored at a fixed frame-boundary hook,
// byte-exact exclude + preserve ranges -- but stores memory as 4 KiB pages shared between
// snapshots: a capture copies only the pages that changed since the previous capture, every
// unchanged page is a reference to the previous snapshot's buffer. The first capture is a full
// copy; every later one costs (dirty pages x 4 KiB).
//
// Dirty detection (selectable, both exact):
//  - Compare:      memcmp each tracked page against its last captured version. Catches every
//                  writer (EE recompiler/interpreter, DMA, IOP-side copies, PINE) with no hooks.
//  - WriteProtect: host page protection on the tracked EE pages (eeMem->Main + fastmem views),
//                  first write per page per frame faults once and marks the page dirty
//                  (vtlb.cpp page-fault handler, same mechanism as recompiler code protection).
//                  Cost scales with the number of dirty pages, not with the region size.
//
// Excluded byte ranges are never rolled back (they keep running linearly, like Slippi's exclude
// list): their live bytes are saved before a load and put back afterwards.
//
// All methods: EE/CPU thread only (frame-boundary hook), except the Stats accessors.
class PageSnapshotRing
{
public:
	enum class DirtyMode : u8
	{
		Compare,
		WriteProtect,
	};

	struct Range
	{
		u32 address = 0; // EE physical
		u32 length = 0;
	};

	struct Stats
	{
		u32 tracked_pages = 0;
		u32 snapshots = 0;
		u32 last_dirty_pages = 0;    // pages copied by the last capture
		u32 last_restored_pages = 0; // pages written by the last load
		u64 last_capture_us = 0;
		u64 last_load_us = 0;
		u32 hot_pages = 0;           // write-protect mode: pages kept writable and compared instead
		u64 pool_bytes = 0; // all page buffers currently allocated (live + free list)
		u64 live_bytes = 0; // unique page buffers referenced by snapshots
		// capture sub-step totals (ns) and count, for optimization
		u64 cap_n = 0, cap_collect_ns = 0, cap_table_ns = 0, cap_copy_ns = 0, cap_evict_ns = 0, cap_hot_ns = 0;
	};

	// regions: EE ranges to snapshot (rounded out to 4 KiB pages). excludes: byte ranges inside
	// them that must never be rolled back. capacity: ring size (Slippi: ROLLBACK_MAX_FRAMES = 7).
	PageSnapshotRing(std::vector<Range> regions, std::vector<Range> excludes, u32 capacity, DirtyMode mode);
	~PageSnapshotRing();

	PageSnapshotRing(const PageSnapshotRing&) = delete;
	PageSnapshotRing& operator=(const PageSnapshotRing&) = delete;

	// Store the current memory as `frame`. Re-capturing an existing frame replaces it; when the
	// ring is full the oldest snapshot is dropped.
	void Capture(s32 frame);

	// Restore memory to `frame` (must exist), keeping `preserve` ranges live (Slippi preserve
	// blocks, e.g. the netplay input buffer). Snapshots newer than `frame` are discarded.
	bool Load(s32 frame, const std::vector<Range>& preserve = {});

	bool Has(s32 frame) const;
	void Clear();

	// Sync-test support: pin a snapshot's page buffers (no copy; they survive the snapshot being dropped), then
	// compare against the newest snapshot page by page -- a page whose buffer is shared is identical by
	// construction and needs no memcmp. Handles are opaque page buffers (PageData() = 4 KiB of bytes).
	bool Pin(s32 frame, std::vector<u32>& out);
	void Unpin(std::vector<u32>& pages);
	bool NewestPages(s32 frame, std::vector<u32>& out) const; // newest snapshot must be `frame`
	const u8* PageData(u32 id) const;
	const std::vector<u32>& PageIndex() const { return m_page_index; }
	// Dirty-page census: how many captures copied each tracked page (EE page address, count), most first.
	std::vector<std::pair<u32, u32>> TopDirtyPages(u32 n) const;

	DirtyMode Mode() const { return m_mode; }
	const Stats& GetStats() const { return m_stats; }
	std::string Describe() const;

private:
	struct Page;
	struct Snapshot
	{
		s32 frame = 0;
		std::vector<u32> pages;       // one page-buffer id per tracked page
		std::vector<u64> dirty_bits;  // pages that differ from the previous snapshot
	};

	// Page buffers are addressed by compact ids; refcounts live in one contiguous array so the per-capture Ref and
	// per-drop Unref loops (one per tracked page per snapshot) never touch the 4 KiB buffers themselves.
	u32 AllocPage();
	__fi void Ref(u32 id) { m_refs[id]++; }
	__fi void Unref(u32 id)
	{
		if (--m_refs[id] == 0)
			m_free.push_back(id);
	}
	__fi u8* Data(u32 id) const;
	void DropSnapshot(Snapshot& s);
	void CollectDirty(std::vector<u64>& bits);
	void ArmWriteProtect();
	void UpdateLiveBytes();
	void UpdateHotPages(const std::vector<u64>& dirty);
	std::vector<u64> HotRamBits() const;

	std::vector<u32> m_page_index; // tracked EE page numbers (addr >> 12), sorted
	std::vector<Range> m_excludes;
	// Exclude ranges split into per-tracked-page segments, computed once (a load keeps their live bytes).
	struct KeepSeg
	{
		u32 address; // EE physical
		u32 length;
		u32 index;   // tracked page index
		u32 offset;  // into m_keep_buf
	};
	std::vector<KeepSeg> m_keep;
	std::vector<u8> m_keep_buf;
	std::vector<Snapshot> m_snap_pool; // recycled snapshot storage (keeps vector capacity)
	std::vector<Snapshot> m_ring; // oldest first
	std::vector<Page*> m_store; // id -> buffer
	std::vector<u32> m_refs;    // id -> snapshots (and pins) referencing it
	std::vector<u32> m_free;    // free ids
	u32 m_capacity;
	DirtyMode m_mode;
	u32 m_words; // u64 words per dirty bitset

	// Write-protect mode, adaptive: a page written on consecutive captures is "hot" -- it stays writable and
	// is checked by memcmp against its last captured copy, because a fault + re-protect per frame costs far
	// more than comparing 4 KiB. A hot page that stays unchanged for HOT_COOLDOWN captures is re-protected.
	static constexpr u8 HOT_PROMOTE = 1;
	static constexpr u8 HOT_COOLDOWN = 30;
	std::vector<u64> m_hot;          // bit per tracked index
	std::vector<u8> m_dirty_streak;  // consecutive dirty captures
	std::vector<u8> m_clean_streak;  // consecutive clean captures while hot
	std::vector<u32> m_dirty_count;  // captures that copied each tracked page (census)
	std::vector<u64> m_streak_nz;    // tracked pages with a nonzero dirty streak (UpdateHotPages visits only these)
	std::vector<s32> m_ram_to_index; // EE RAM page -> tracked index (-1 = untracked)
	Stats m_stats;
};
