// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Sdbz/PageSnapshotRing.h"

#include "Memory.h" // eeMem
#include "vtlb.h"

#include "common/Console.h"
#include "common/Timer.h"
#include "Sdbz/CpuTimer.h"

#include "fmt/format.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

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

namespace
{
	// Parallel page copy for captures. A rollback captures after every re-simulated frame, so jobs arrive back to
	// back: workers spin briefly after a job (no wake-up latency inside a rollback burst), then sleep. The calling
	// thread copies a share too. Small jobs are copied inline (handoff would cost more than it saves).
	class CopyPool
	{
	public:
		struct Job
		{
			void* dst;
			const void* src;
		};

		static CopyPool& Get()
		{
			static CopyPool pool;
			return pool;
		}

		void Run(const std::vector<Job>& jobs, u32 bytes_each) { Dispatch(jobs, bytes_each, nullptr); }

		// Parallel memcmp: differs[i] = (jobs[i].dst != jobs[i].src) over bytes_each.
		void Compare(const std::vector<Job>& jobs, std::vector<u8>& differs, u32 bytes_each)
		{
			differs.assign(jobs.size(), 0);
			Dispatch(jobs, bytes_each, differs.data());
		}

	private:
		void Dispatch(const std::vector<Job>& jobs, u32 bytes_each, u8* differs)
		{
			const u32 n = static_cast<u32>(jobs.size());
			if (n < MIN_PARALLEL || m_workers.empty())
			{
				for (u32 i = 0; i < n; i++)
					Do(jobs[i], i, bytes_each, differs);
				return;
			}
			m_differs = differs;
			m_jobs = jobs.data();
			m_count = n;
			m_bytes = bytes_each;
			m_next.store(0, std::memory_order_relaxed);
			m_done.store(0, std::memory_order_relaxed);
			{
				std::lock_guard lk(m_mtx);
				m_gen.fetch_add(1, std::memory_order_release);
			}
			m_cv.notify_all();
			Work();
			while (m_done.load(std::memory_order_acquire) < n)
				std::this_thread::yield();
		}

		static __fi void Do(const Job& j, u32 i, u32 bytes, u8* differs)
		{
			if (differs)
				differs[i] = std::memcmp(j.dst, j.src, bytes) != 0;
			else
				std::memcpy(j.dst, j.src, bytes);
		}

		static constexpr u32 MIN_PARALLEL = 48;  // pages
		static constexpr u32 CHUNK = 16;         // pages per grab
		static constexpr u32 WORKERS = 3;

		CopyPool()
		{
			for (u32 i = 0; i < WORKERS; i++)
				m_workers.emplace_back([this]() { Loop(); });
		}
		~CopyPool()
		{
			m_quit = true;
			{
				std::lock_guard lk(m_mtx);
				m_gen.fetch_add(1);
			}
			m_cv.notify_all();
			for (std::thread& t : m_workers)
				t.join();
		}

		void Work()
		{
			for (;;)
			{
				const u32 first = m_next.fetch_add(CHUNK, std::memory_order_relaxed);
				if (first >= m_count)
					return;
				const u32 last = std::min(first + CHUNK, m_count);
				for (u32 i = first; i < last; i++)
					Do(m_jobs[i], i, m_bytes, m_differs);
				m_done.fetch_add(last - first, std::memory_order_release);
			}
		}

		void Loop()
		{
			u64 seen = 0;
			for (;;)
			{
				// spin ~2 ms for the next job (rollback bursts), then sleep
				const auto spin_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
				while (m_gen.load(std::memory_order_acquire) == seen && std::chrono::steady_clock::now() < spin_until)
					std::this_thread::yield();
				if (m_gen.load(std::memory_order_acquire) == seen)
				{
					std::unique_lock lk(m_mtx);
					m_cv.wait(lk, [&]() { return m_gen.load() != seen; });
				}
				seen = m_gen.load(std::memory_order_acquire);
				if (m_quit)
					return;
				Work();
			}
		}

		std::vector<std::thread> m_workers;
		std::mutex m_mtx;
		std::condition_variable m_cv;
		std::atomic<u64> m_gen{0};
		std::atomic<u32> m_next{0}, m_done{0};
		const Job* m_jobs = nullptr;
		u8* m_differs = nullptr;
		u32 m_count = 0, m_bytes = 0;
		std::atomic<bool> m_quit{false};
	};
} // namespace

struct PageSnapshotRing::Page
{
	alignas(64) u8 data[PAGE_SIZE];
};

__fi u8* PageSnapshotRing::Data(u32 id) const
{
	return m_store[id]->data;
}

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

	// A page lying entirely inside an exclude range is never rolled back, so don't track it at all: it would
	// otherwise be copied on every capture (render/audio buffers are dirty every frame) and re-protected on
	// every load. Only partially excluded pages stay tracked; their excluded bytes are preserved on load.
	auto fully_excluded = [this](u32 page) {
		const u32 lo = page << PAGE_SHIFT, hi = lo + PAGE_SIZE;
		return std::any_of(m_excludes.begin(), m_excludes.end(), [lo, hi](const Range& e) {
			const u32 a = e.address & RAM_MASK;
			return a <= lo && a + e.length >= hi;
		});
	};
	pages.erase(std::remove_if(pages.begin(), pages.end(), fully_excluded), pages.end());
	m_page_index = std::move(pages);
	m_words = static_cast<u32>((m_page_index.size() + 63) / 64);
	m_hot.assign(m_words, 0);
	m_dirty_streak.assign(m_page_index.size(), 0);
	m_clean_streak.assign(m_page_index.size(), 0);
	m_dirty_count.assign(m_page_index.size(), 0);
	m_streak_nz.assign(m_words, 0);
	m_ram_to_index.assign(Ps2MemSize::TotalRam >> PAGE_SHIFT, -1);
	for (u32 i = 0; i < m_page_index.size(); i++)
		m_ram_to_index[m_page_index[i]] = static_cast<s32>(i);
	m_stats.tracked_pages = static_cast<u32>(m_page_index.size());

	// Exclude ranges -> per-tracked-page segments, once. Every load keeps these live bytes.
	u32 keep_bytes = 0;
	for (const Range& r : m_excludes)
	{
		u32 a = r.address & RAM_MASK;
		const u32 end = a + r.length;
		while (a < end)
		{
			const u32 page = a >> PAGE_SHIFT;
			const u32 next = std::min(end, (page + 1) << PAGE_SHIFT);
			const auto it = std::lower_bound(m_page_index.begin(), m_page_index.end(), page);
			if (it != m_page_index.end() && *it == page)
			{
				m_keep.push_back({a, next - a, static_cast<u32>(it - m_page_index.begin()), keep_bytes});
				keep_bytes += next - a;
			}
			a = next;
		}
	}
	m_keep_buf.resize(keep_bytes);

	if (m_mode == DirtyMode::WriteProtect)
	{
		vtlb_DirtyTrack_Enable(m_page_index);
		// Map the writable alias now and touch every tracked page through it once, so the first rollback load
		// doesn't pay for the view mapping + a soft page fault per page (measured: first load 12.7 ms vs 0.2-0.4 ms).
		if (volatile u8* alias = SysMemory::GetEEMainWritableAlias())
		{
			u32 sum = 0;
			for (const u32 page : m_page_index)
				sum += alias[page << PAGE_SHIFT];
			(void)sum;
		}
	}

	Console.WriteLn("PageSnapshotRing: %u pages (%u KiB), capacity %u, %s", m_stats.tracked_pages,
		m_stats.tracked_pages * (PAGE_SIZE / 1024), m_capacity, m_mode == DirtyMode::Compare ? "compare" : "write-protect");
}

PageSnapshotRing::~PageSnapshotRing()
{
	if (m_mode == DirtyMode::WriteProtect)
		vtlb_DirtyTrack_Disable();
	Clear();
	for (Page* p : m_store)
		delete p;
}

u32 PageSnapshotRing::AllocPage()
{
	u32 id;
	if (!m_free.empty())
	{
		id = m_free.back();
		m_free.pop_back();
	}
	else
	{
		id = static_cast<u32>(m_store.size());
		m_store.push_back(new Page());
		m_refs.push_back(0);
		m_stats.pool_bytes += sizeof(Page);
	}
	m_refs[id] = 1;
	return id;
}

void PageSnapshotRing::DropSnapshot(Snapshot& s)
{
	for (const u32 id : s.pages)
		Unref(id);
	s.pages.clear(); // capacity kept: the storage is recycled through m_snap_pool
}

void PageSnapshotRing::Clear()
{
	for (Snapshot& s : m_ring)
		DropSnapshot(s);
	m_ring.clear();
	m_stats.snapshots = 0;
	m_stats.live_bytes = 0;
}

bool PageSnapshotRing::Pin(s32 frame, std::vector<u32>& out)
{
	out.clear();
	auto it = std::find_if(m_ring.begin(), m_ring.end(), [frame](const Snapshot& s) { return s.frame == frame; });
	if (it == m_ring.end())
		return false;
	out = it->pages;
	for (const u32 id : out)
		Ref(id);
	return true;
}

void PageSnapshotRing::Unpin(std::vector<u32>& pages)
{
	for (const u32 id : pages)
		Unref(id);
	pages.clear();
	UpdateLiveBytes();
}

bool PageSnapshotRing::NewestPages(s32 frame, std::vector<u32>& out) const
{
	out.clear();
	if (m_ring.empty() || m_ring.back().frame != frame)
		return false;
	out = m_ring.back().pages;
	return true;
}

std::vector<std::pair<u32, u32>> PageSnapshotRing::TopDirtyPages(u32 n) const
{
	std::vector<std::pair<u32, u32>> v;
	for (u32 i = 0; i < m_page_index.size(); i++)
		if (m_dirty_count[i])
			v.push_back({m_dirty_count[i], m_page_index[i] << PAGE_SHIFT});
	std::sort(v.rbegin(), v.rend());
	if (v.size() > n)
		v.resize(n);
	for (auto& e : v)
		std::swap(e.first, e.second); // (address, count)
	return v;
}

const u8* PageSnapshotRing::PageData(u32 id) const
{
	return Data(id);
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
			if (std::memcmp(RamPage(m_page_index[i]), Data(latest.pages[i]), PAGE_SIZE) != 0)
				SetBit(bits, i);
		}
		return;
	}

	// WriteProtect: ram-page bitset from the fault handler -> tracked-index bitset; hot pages (kept writable,
	// so they no longer fault) are compared against their last captured copy instead.
	std::vector<u64> ram_dirty;
	const std::vector<u64> hot_ram = HotRamBits();
	vtlb_DirtyTrack_Rearm(&ram_dirty, &hot_ram);
	static std::vector<CopyPool::Job> hot_jobs;
	static std::vector<u32> hot_idx;
	static std::vector<u8> differs;
	hot_jobs.clear();
	hot_idx.clear();
	for (u32 w = 0; w < m_words; w++)
	{
		u64 b = m_hot[w];
		while (b)
		{
			const u32 i = w * 64 + static_cast<u32>(std::countr_zero(b));
			b &= b - 1;
			hot_idx.push_back(i);
			hot_jobs.push_back({RamPage(m_page_index[i]), Data(latest.pages[i])});
		}
	}
	CopyPool::Get().Compare(hot_jobs, differs, PAGE_SIZE); // hot pages don't fault: compare with the last copy
	for (size_t k = 0; k < hot_idx.size(); k++)
		if (differs[k])
			SetBit(bits, hot_idx[k]);
	// Fault-dirty pages: iterate the (few) RAM pages the fault handler flagged, not every tracked page.
	for (u32 w = 0; w < ram_dirty.size(); w++)
	{
		u64 b = ram_dirty[w];
		while (b)
		{
			const u32 page = w * 64 + static_cast<u32>(std::countr_zero(b));
			b &= b - 1;
			const s32 i = (page < m_ram_to_index.size()) ? m_ram_to_index[page] : -1;
			if (i >= 0 && !TestBit(m_hot, static_cast<u32>(i)))
				SetBit(bits, static_cast<u32>(i));
		}
	}
}

std::vector<u64> PageSnapshotRing::HotRamBits() const
{
	std::vector<u64> ram((Ps2MemSize::TotalRam >> PAGE_SHIFT) / 64, 0);
	for (u32 w = 0; w < m_words; w++)
	{
		u64 b = m_hot[w];
		while (b)
		{
			const u32 page = m_page_index[w * 64 + static_cast<u32>(std::countr_zero(b))];
			b &= b - 1;
			ram[page >> 6] |= 1ull << (page & 63);
		}
	}
	return ram;
}

void PageSnapshotRing::UpdateHotPages(const std::vector<u64>& dirty)
{
	if (m_mode != DirtyMode::WriteProtect)
		return;
	// Only pages that are dirty now, hot, or were dirty last time can change state: visit those bits only.
	for (u32 w = 0; w < m_words; w++)
	{
		u64 cand = dirty[w] | m_hot[w] | m_streak_nz[w];
		m_streak_nz[w] = 0;
		while (cand)
		{
			const u32 i = w * 64 + static_cast<u32>(std::countr_zero(cand));
			cand &= cand - 1;
			const bool d = (dirty[w] >> (i & 63)) & 1;
			m_dirty_streak[i] = d ? static_cast<u8>(std::min(255, m_dirty_streak[i] + 1)) : 0;
			if (m_dirty_streak[i])
				m_streak_nz[w] |= 1ull << (i & 63);
			if (TestBit(m_hot, i))
			{
				m_clean_streak[i] = d ? 0 : static_cast<u8>(std::min(255, m_clean_streak[i] + 1));
				if (m_clean_streak[i] >= HOT_COOLDOWN)
				{
					m_hot[w] &= ~(1ull << (i & 63));
					m_stats.hot_pages--;
					vtlb_DirtyTrack_Reprotect(m_page_index[i]); // back to fault-based tracking
				}
			}
			else if (m_dirty_streak[i] >= HOT_PROMOTE)
			{
				SetBit(m_hot, i); // stays writable from the next rearm on
				m_clean_streak[i] = 0;
				m_stats.hot_pages++;
			}
		}
	}
}

void PageSnapshotRing::Capture(s32 frame)
{
	if (!eeMem)
		return;

	CpuTimer timer;

	std::vector<u64> dirty;
	CpuTimer st;
	CollectDirty(dirty);
	m_stats.cap_collect_ns += static_cast<u64>(st.GetTimeNanoseconds());
	st.Reset();

	Snapshot snap;
	if (!m_snap_pool.empty())
	{
		snap = std::move(m_snap_pool.back());
		m_snap_pool.pop_back();
	}
	snap.frame = frame;
	snap.dirty_bits.assign(dirty.begin(), dirty.end());
	snap.pages.resize(m_page_index.size());

	const Snapshot* base = m_ring.empty() ? nullptr : &m_ring.back();
	const u32 n = static_cast<u32>(m_page_index.size());
	// Share every page of the base (one table copy + a tight refcount pass), then give the dirty ones new buffers.
	if (base)
	{
		std::memcpy(snap.pages.data(), base->pages.data(), n * sizeof(u32));
		for (u32 i = 0; i < n; i++)
			m_refs[snap.pages[i]]++;
	}
	static std::vector<CopyPool::Job> jobs;
	jobs.clear();
	auto take = [&](u32 i) {
		if (base)
			Unref(snap.pages[i]); // base keeps its own reference
		const u32 id = AllocPage();
		snap.pages[i] = id;
		m_dirty_count[i]++;
		jobs.push_back({Data(id), RamPage(m_page_index[i])});
	};
	if (base)
	{
		for (u32 w = 0; w < m_words; w++)
		{
			u64 bits = dirty[w];
			while (bits)
			{
				take(w * 64 + static_cast<u32>(std::countr_zero(bits)));
				bits &= bits - 1;
			}
		}
	}
	else
	{
		for (u32 i = 0; i < n; i++)
			take(i);
	}
	m_stats.cap_table_ns += static_cast<u64>(st.GetTimeNanoseconds());
	st.Reset();
	Common::Timer copy_wall; // wall time: includes waiting for the copy workers
	CopyPool::Get().Run(jobs, PAGE_SIZE);
	const u32 copied = static_cast<u32>(jobs.size());
	m_stats.cap_copy_ns += static_cast<u64>(copy_wall.GetTimeNanoseconds());
	st.Reset();

	// Remove an older entry for the same frame, then evict the oldest if over capacity.
	for (auto it = m_ring.begin(); it != m_ring.end(); ++it)
	{
		if (it->frame == frame)
		{
			DropSnapshot(*it);
			m_snap_pool.push_back(std::move(*it));
			m_ring.erase(it);
			break;
		}
	}
	m_ring.push_back(std::move(snap));
	while (m_ring.size() > m_capacity)
	{
		DropSnapshot(m_ring.front());
		m_snap_pool.push_back(std::move(m_ring.front()));
		m_ring.erase(m_ring.begin());
	}

	m_stats.cap_evict_ns += static_cast<u64>(st.GetTimeNanoseconds());
	st.Reset();
	UpdateHotPages(dirty);
	m_stats.cap_hot_ns += static_cast<u64>(st.GetTimeNanoseconds());
	m_stats.cap_n++;
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

	CpuTimer timer;

	// Pages that may differ from the target: everything written since the newest snapshot, plus
	// every page that changed in any snapshot after the target.
	std::vector<u64> restore;
	CollectDirty(restore);
	for (auto later = it + 1; later != m_ring.end(); ++later)
	{
		for (u32 w = 0; w < m_words; w++)
			restore[w] |= later->dirty_bits[w];
	}

	// Live bytes that must survive the rewind: excludes (never rolled back, precomputed per-page segments in
	// m_keep) + caller preserves. Only segments in pages this load restores need saving: other pages keep their
	// live bytes untouched.
	std::vector<KeepSeg> extra; // caller preserves (rare): computed per call
	u32 extra_bytes = 0;
	for (const Range& r : preserve)
	{
		u32 a = r.address & RAM_MASK;
		const u32 end = a + r.length;
		while (a < end)
		{
			const u32 page = a >> PAGE_SHIFT;
			const u32 next = std::min(end, (page + 1) << PAGE_SHIFT);
			const auto pit = std::lower_bound(m_page_index.begin(), m_page_index.end(), page);
			if (pit != m_page_index.end() && *pit == page)
			{
				extra.push_back({a, next - a, static_cast<u32>(pit - m_page_index.begin()),
					static_cast<u32>(m_keep_buf.size()) + extra_bytes});
				extra_bytes += next - a;
			}
			a = next;
		}
	}
	std::vector<u8> extra_buf(extra_bytes);
	auto seg_buf = [this, &extra_buf](const KeepSeg& k) -> u8* {
		return (k.offset < m_keep_buf.size()) ? &m_keep_buf[k.offset] : &extra_buf[k.offset - m_keep_buf.size()];
	};
	auto restored_page = [&restore](u32 i) { return (restore[i >> 6] >> (i & 63)) & 1; };
	auto save_keep = [&](const KeepSeg& k) {
		if (restored_page(k.index))
			std::memcpy(seg_buf(k), &eeMem->Main[k.address], k.length);
	};
	for (const KeepSeg& k : m_keep)
		save_keep(k);
	for (const KeepSeg& k : extra)
		save_keep(k);

	// Write-protect mode: data pages are restored through a writable alias of EE RAM, so their protection
	// never changes (they end up protected and clean relative to the target). Pages holding recompiled code
	// must be written through the protected view so the recompiler drops its blocks; without an alias every
	// page takes that path (contiguous runs unprotected with one call).
	u8* const alias = (m_mode == DirtyMode::WriteProtect) ? SysMemory::GetEEMainWritableAlias() : nullptr;
	auto dest_for = [alias](u32 page) -> u8* {
		return (alias && !vtlb_IsCodeProtectedPage(page)) ? alias + (page << PAGE_SHIFT) : nullptr;
	};

	u32 restored = 0;
	u32 run_first = 0, run_len = 0; // consecutive RAM pages -> one unprotect call
	auto flush_run = [&]() {
		if (run_len && m_mode == DirtyMode::WriteProtect)
			vtlb_DirtyTrack_Unprotect(run_first, run_len);
		run_len = 0;
	};
	std::vector<u32> via_main;
	for (u32 w = 0; w < m_words; w++)
	{
		u64 bits = restore[w];
		while (bits)
		{
			const u32 i = w * 64 + static_cast<u32>(std::countr_zero(bits));
			bits &= bits - 1;
			const u32 page = m_page_index[i];
			restored++;
			if (u8* dst = dest_for(page))
			{
				std::memcpy(dst, Data(it->pages[i]), PAGE_SIZE);
				continue;
			}
			if (run_len && page == run_first + run_len)
				run_len++;
			else
			{
				flush_run();
				run_first = page;
				run_len = 1;
			}
			via_main.push_back(i);
		}
	}
	flush_run();
	for (const u32 i : via_main)
		std::memcpy(RamPage(m_page_index[i]), Data(it->pages[i]), PAGE_SIZE);

	// Put the kept live bytes back (restored pages only). A page whose kept bytes differ from the target
	// snapshot's must be captured again; one where they are equal is clean relative to the target.
	std::vector<u32> keep_dirty_pages;
	auto restore_keep = [&](const KeepSeg& k) {
		if (!restored_page(k.index))
			return;
		const u8* src = seg_buf(k);
		const u32 a = k.address;
		if (std::memcmp(src, Data(it->pages[k.index]) + (a & (PAGE_SIZE - 1)), k.length) == 0)
			return; // live bytes == target bytes: the page as restored is already right
		keep_dirty_pages.push_back(a >> PAGE_SHIFT);
		if (u8* dst = dest_for(a >> PAGE_SHIFT))
		{
			std::memcpy(dst + (a & (PAGE_SIZE - 1)), src, k.length);
			return;
		}
		if (m_mode == DirtyMode::WriteProtect)
			vtlb_DirtyTrack_Unprotect(a >> PAGE_SHIFT);
		std::memcpy(&eeMem->Main[a], src, k.length);
	};
	for (const KeepSeg& k : m_keep)
		restore_keep(k);
	for (const KeepSeg& k : extra)
		restore_keep(k);

	// The target becomes the newest snapshot and the base for the next capture. Memory now equals
	// it except for the kept ranges, so those pages must count as dirty for the next capture.
	for (auto later = it + 1; later != m_ring.end(); ++later)
	{
		DropSnapshot(*later);
		m_snap_pool.push_back(std::move(*later));
	}
	m_ring.erase(it + 1, m_ring.end());

	if (m_mode == DirtyMode::WriteProtect)
	{
		// Reset fault tracking to "clean vs target" (hot pages stay writable and are compared), then re-flag
		// the kept ranges' pages.
		const std::vector<u64> hot_ram = HotRamBits();
		vtlb_DirtyTrack_Rearm(nullptr, &hot_ram);
		// Pages whose kept (never-rolled-back) bytes differ from the target must be copied by the next capture.
		for (const u32 page : keep_dirty_pages)
			vtlb_DirtyTrack_MarkDirty(page);
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
					   "live {} KiB, pool {} KiB | hot {} | {}",
		m_stats.snapshots, m_stats.tracked_pages * 4, m_stats.last_dirty_pages, m_stats.last_dirty_pages * 4,
		m_stats.last_capture_us, m_stats.last_restored_pages, m_stats.last_load_us, m_stats.live_bytes / 1024,
		m_stats.pool_bytes / 1024, m_stats.hot_pages, m_mode == DirtyMode::Compare ? "compare" : "write-protect") +
		(m_stats.cap_n ? fmt::format(" | capture avg us: collect {:.0f} table {:.0f} copy {:.0f} evict {:.0f} hot {:.0f}",
							 m_stats.cap_collect_ns / 1000.0 / m_stats.cap_n, m_stats.cap_table_ns / 1000.0 / m_stats.cap_n,
							 m_stats.cap_copy_ns / 1000.0 / m_stats.cap_n, m_stats.cap_evict_ns / 1000.0 / m_stats.cap_n,
							 m_stats.cap_hot_ns / 1000.0 / m_stats.cap_n) : std::string());
}
