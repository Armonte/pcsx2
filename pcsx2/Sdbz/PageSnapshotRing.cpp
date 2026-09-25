// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Sdbz/PageSnapshotRing.h"

#include "Memory.h" // eeMem
#include "vtlb.h"

#include "common/Console.h"
#include "common/Timer.h"

#include "fmt/format.h"

#include <algorithm>
#include <bit>
#include <cstring>

namespace
{
	constexpr u32 PAGE_SHIFT = 12;
	constexpr u32 PAGE_SIZE = 1u << PAGE_SHIFT;
	constexpr u32 RAM_MASK = 0x01FFFFFFu; // EE physical -> eeMem->Main offset (32 MiB main RAM)

	__fi u8* RamPage(u32 page)
	{
		return &eeMem->Main[page << PAGE_SHIFT];
	}

	__fi bool TestBit(const std::vector<u64>& bits, u32 i)
	{
		return (bits[i >> 6] >> (i & 63)) & 1;
	}

	__fi void SetBit(std::vector<u64>& bits, u32 i)
	{
		bits[i >> 6] |= 1ull << (i & 63);
	}
} // namespace

struct PageSnapshotRing::Page
{
	alignas(64) u8 data[PAGE_SIZE];
	u32 refs = 0;
};

PageSnapshotRing::PageSnapshotRing(std::vector<Range> regions, std::vector<Range> excludes, u32 capacity, DirtyMode mode)
	: m_excludes(std::move(excludes))
	, m_capacity(std::max(capacity, 1u))
	, m_mode(mode)
{
	std::vector<u32> pages;
	for (const Range& r : regions)
	{
		if (r.length == 0)
			continue;
		const u32 first = (r.address & RAM_MASK) >> PAGE_SHIFT;
		const u32 last = ((r.address & RAM_MASK) + r.length - 1) >> PAGE_SHIFT;
		for (u32 p = first; p <= last; p++)
			pages.push_back(p);
	}
	std::sort(pages.begin(), pages.end());
	pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
	m_page_index = std::move(pages);
	m_words = static_cast<u32>((m_page_index.size() + 63) / 64);
	m_stats.tracked_pages = static_cast<u32>(m_page_index.size());

	if (m_mode == DirtyMode::WriteProtect)
		vtlb_DirtyTrack_Enable(m_page_index);

	Console.WriteLn("PageSnapshotRing: %u pages (%u KiB), capacity %u, %s", m_stats.tracked_pages,
		m_stats.tracked_pages * (PAGE_SIZE / 1024), m_capacity, m_mode == DirtyMode::Compare ? "compare" : "write-protect");
}

PageSnapshotRing::~PageSnapshotRing()
{
	if (m_mode == DirtyMode::WriteProtect)
		vtlb_DirtyTrack_Disable();
	Clear();
	for (Page* p : m_free)
		delete p;
}

PageSnapshotRing::Page* PageSnapshotRing::AllocPage()
{
	Page* p;
	if (!m_free.empty())
	{
		p = m_free.back();
		m_free.pop_back();
	}
	else
	{
		p = new Page();
		m_stats.pool_bytes += sizeof(Page);
	}
	p->refs = 1;
	return p;
}

void PageSnapshotRing::Ref(Page* p)
{
	p->refs++;
}

void PageSnapshotRing::Unref(Page* p)
{
	if (--p->refs == 0)
		m_free.push_back(p);
}

void PageSnapshotRing::DropSnapshot(Snapshot& s)
{
	for (Page* p : s.pages)
		Unref(p);
	s.pages.clear();
}

void PageSnapshotRing::Clear()
{
	for (Snapshot& s : m_ring)
		DropSnapshot(s);
	m_ring.clear();
	m_stats.snapshots = 0;
	m_stats.live_bytes = 0;
}

bool PageSnapshotRing::Has(s32 frame) const
{
	return std::any_of(m_ring.begin(), m_ring.end(), [frame](const Snapshot& s) { return s.frame == frame; });
}

// Dirty set of the current memory relative to the newest snapshot (bit = tracked page index).
void PageSnapshotRing::CollectDirty(std::vector<u64>& bits)
{
	bits.assign(m_words, 0);
	if (m_ring.empty())
	{
		for (u32 i = 0; i < m_page_index.size(); i++)
			SetBit(bits, i);
		if (m_mode == DirtyMode::WriteProtect)
			vtlb_DirtyTrack_Rearm(nullptr);
		return;
	}

	const Snapshot& latest = m_ring.back();
	if (m_mode == DirtyMode::Compare)
	{
		for (u32 i = 0; i < m_page_index.size(); i++)
		{
			if (std::memcmp(RamPage(m_page_index[i]), latest.pages[i]->data, PAGE_SIZE) != 0)
				SetBit(bits, i);
		}
		return;
	}

	// WriteProtect: ram-page bitset from the fault handler -> tracked-index bitset.
	std::vector<u64> ram_dirty;
	vtlb_DirtyTrack_Rearm(&ram_dirty);
	for (u32 i = 0; i < m_page_index.size(); i++)
	{
		const u32 page = m_page_index[i];
		if ((ram_dirty[page >> 6] >> (page & 63)) & 1)
			SetBit(bits, i);
	}
}

void PageSnapshotRing::Capture(s32 frame)
{
	if (!eeMem)
		return;

	Common::Timer timer;

	std::vector<u64> dirty;
	CollectDirty(dirty);

	Snapshot snap;
	snap.frame = frame;
	snap.dirty_bits = dirty;
	snap.pages.resize(m_page_index.size());

	const Snapshot* base = m_ring.empty() ? nullptr : &m_ring.back();
	u32 copied = 0;
	for (u32 i = 0; i < m_page_index.size(); i++)
	{
		if (base && !TestBit(dirty, i))
		{
			snap.pages[i] = base->pages[i];
			Ref(snap.pages[i]);
			continue;
		}
		Page* p = AllocPage();
		std::memcpy(p->data, RamPage(m_page_index[i]), PAGE_SIZE);
		snap.pages[i] = p;
		copied++;
	}

	// Remove an older entry for the same frame, then evict the oldest if over capacity.
	for (auto it = m_ring.begin(); it != m_ring.end(); ++it)
	{
		if (it->frame == frame)
		{
			DropSnapshot(*it);
			m_ring.erase(it);
			break;
		}
	}
	m_ring.push_back(std::move(snap));
	while (m_ring.size() > m_capacity)
	{
		DropSnapshot(m_ring.front());
		m_ring.erase(m_ring.begin());
	}

	m_stats.snapshots = static_cast<u32>(m_ring.size());
	m_stats.last_dirty_pages = copied;
	m_stats.last_capture_us = static_cast<u64>(timer.GetTimeNanoseconds() / 1000.0);
	UpdateLiveBytes();
}

bool PageSnapshotRing::Load(s32 frame, const std::vector<Range>& preserve)
{
	if (!eeMem)
		return false;

	auto it = std::find_if(m_ring.begin(), m_ring.end(), [frame](const Snapshot& s) { return s.frame == frame; });
	if (it == m_ring.end())
		return false;

	Common::Timer timer;

	// Pages that may differ from the target: everything written since the newest snapshot, plus
	// every page that changed in any snapshot after the target.
	std::vector<u64> restore;
	CollectDirty(restore);
	for (auto later = it + 1; later != m_ring.end(); ++later)
	{
		for (u32 w = 0; w < m_words; w++)
			restore[w] |= later->dirty_bits[w];
	}

	// Live bytes that must survive the rewind: excludes (never rolled back) + caller preserves.
	std::vector<Range> keep = m_excludes;
	keep.insert(keep.end(), preserve.begin(), preserve.end());
	std::vector<std::vector<u8>> kept(keep.size());
	for (size_t k = 0; k < keep.size(); k++)
	{
		kept[k].resize(keep[k].length);
		std::memcpy(kept[k].data(), &eeMem->Main[keep[k].address & RAM_MASK], keep[k].length);
	}

	u32 restored = 0;
	for (u32 w = 0; w < m_words; w++)
	{
		u64 bits = restore[w];
		while (bits)
		{
			const u32 i = w * 64 + static_cast<u32>(std::countr_zero(bits));
			bits &= bits - 1;
			const u32 page = m_page_index[i];
			if (m_mode == DirtyMode::WriteProtect)
				vtlb_DirtyTrack_Unprotect(page);
			std::memcpy(RamPage(page), it->pages[i]->data, PAGE_SIZE);
			restored++;
		}
	}

	for (size_t k = 0; k < keep.size(); k++)
	{
		const u32 page = (keep[k].address & RAM_MASK) >> PAGE_SHIFT;
		const u32 last = ((keep[k].address & RAM_MASK) + keep[k].length - 1) >> PAGE_SHIFT;
		if (m_mode == DirtyMode::WriteProtect)
		{
			for (u32 p = page; p <= last; p++)
				vtlb_DirtyTrack_Unprotect(p);
		}
		std::memcpy(&eeMem->Main[keep[k].address & RAM_MASK], kept[k].data(), keep[k].length);
	}

	// The target becomes the newest snapshot and the base for the next capture. Memory now equals
	// it except for the kept ranges, so those pages must count as dirty for the next capture.
	for (auto later = it + 1; later != m_ring.end(); ++later)
		DropSnapshot(*later);
	m_ring.erase(it + 1, m_ring.end());

	if (m_mode == DirtyMode::WriteProtect)
	{
		// Reset fault tracking to "clean vs target", then re-flag the kept ranges' pages.
		vtlb_DirtyTrack_Rearm(nullptr);
		for (const Range& r : keep)
		{
			const u32 first = (r.address & RAM_MASK) >> PAGE_SHIFT;
			const u32 last = ((r.address & RAM_MASK) + r.length - 1) >> PAGE_SHIFT;
			for (u32 p = first; p <= last; p++)
				vtlb_DirtyTrack_Unprotect(p);
		}
	}

	m_stats.snapshots = static_cast<u32>(m_ring.size());
	m_stats.last_restored_pages = restored;
	m_stats.last_load_us = static_cast<u64>(timer.GetTimeNanoseconds() / 1000.0);
	UpdateLiveBytes();
	return true;
}

void PageSnapshotRing::UpdateLiveBytes()
{
	// Every allocated page buffer is either referenced by a snapshot or on the free list.
	m_stats.live_bytes = m_stats.pool_bytes - static_cast<u64>(m_free.size()) * sizeof(Page);
}

std::string PageSnapshotRing::Describe() const
{
	return fmt::format("{} snaps | tracked {} KiB | last capture {} pages ({} KiB) {} us | last load {} pages {} us | "
					   "live {} KiB, pool {} KiB | {}",
		m_stats.snapshots, m_stats.tracked_pages * 4, m_stats.last_dirty_pages, m_stats.last_dirty_pages * 4,
		m_stats.last_capture_us, m_stats.last_restored_pages, m_stats.last_load_us, m_stats.live_bytes / 1024,
		m_stats.pool_bytes / 1024, m_mode == DirtyMode::Compare ? "compare" : "write-protect");
}
