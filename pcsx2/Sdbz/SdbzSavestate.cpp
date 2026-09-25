// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Sdbz/SdbzSavestate.h"

#include "Memory.h" // eeMem

#include "common/Console.h"

#include <algorithm>
#include <cstring>

namespace
{
	__fi u8* EePtr(u32 addr)
	{
		return &eeMem->Main[addr & 0x01FFFFFFu];
	}
} // namespace

SdbzSavestate::SdbzSavestate(u32 dataStart, u32 heapEnd, std::vector<PreserveBlock> excludes)
{
	initBackupLocs(dataStart, heapEnd, std::move(excludes));

	u32 total = 0;
	for (BackupLoc& loc : m_backup_locs)
	{
		loc.data.resize(loc.endAddress - loc.startAddress);
		total += loc.endAddress - loc.startAddress;
	}
	Console.WriteLn("SdbzSavestate: %zu region(s), %u bytes total", m_backup_locs.size(), total);
}

u32 SdbzSavestate::TotalBytes() const
{
	u32 total = 0;
	for (const BackupLoc& loc : m_backup_locs)
		total += loc.endAddress - loc.startAddress;
	return total;
}

// Port of SlippiSavestate::initBackupLocs(): start from the full region list, then carve every
// exclude section out (splitting regions as needed). Same algorithm, same edge cases.
void SdbzSavestate::initBackupLocs(u32 dataStart, u32 heapEnd, std::vector<PreserveBlock> excludes)
{
	m_backup_locs.push_back({dataStart, heapEnd, {}});

	std::sort(excludes.begin(), excludes.end());

	size_t idx = 0;
	for (const PreserveBlock& excl : excludes)
	{
		PreserveBlock ipb = excl;

		while (ipb.length > 0)
		{
			// Move up the region index until we reach a section relevant to us
			while (idx < m_backup_locs.size() && ipb.address >= m_backup_locs[idx].endAddress)
				idx++;

			// Once idx is beyond the regions, we are already not backing up this exclusion
			if (idx >= m_backup_locs.size())
				break;

			// Handle case where our exclusion starts before the actual backup section
			if (ipb.address < m_backup_locs[idx].startAddress)
			{
				const s32 newSize = static_cast<s32>(ipb.length) -
									(static_cast<s32>(m_backup_locs[idx].startAddress) - static_cast<s32>(ipb.address));
				ipb.length = (newSize > 0) ? static_cast<u32>(newSize) : 0;
				ipb.address = m_backup_locs[idx].startAddress;
				continue;
			}

			// Determine new size (how much of the exclusion remains past this region)
			s32 newSize = static_cast<s32>(ipb.length) -
						  (static_cast<s32>(m_backup_locs[idx].endAddress) - static_cast<s32>(ipb.address));

			// Add split section after the exclusion
			if (m_backup_locs[idx].endAddress > ipb.address + ipb.length)
			{
				BackupLoc newLoc = {ipb.address + ipb.length, m_backup_locs[idx].endAddress, {}};
				m_backup_locs.insert(m_backup_locs.begin() + idx + 1, std::move(newLoc));
			}

			// Truncate this region to end at the exclusion start
			m_backup_locs[idx].endAddress = ipb.address;
			if (m_backup_locs[idx].endAddress <= m_backup_locs[idx].startAddress)
				m_backup_locs.erase(m_backup_locs.begin() + idx);

			newSize = (newSize > 0) ? newSize : 0;
			ipb.address = ipb.address + (ipb.length - static_cast<u32>(newSize));
			ipb.length = static_cast<u32>(newSize);
		}
	}
}

void SdbzSavestate::Capture()
{
	if (!eeMem)
		return;

	for (BackupLoc& loc : m_backup_locs)
		std::memcpy(loc.data.data(), EePtr(loc.startAddress), loc.endAddress - loc.startAddress);
}

void SdbzSavestate::Load(const std::vector<PreserveBlock>& preserve_blocks)
{
	if (!eeMem)
		return;

	// Back up the caller's live ranges (Slippi's preservationMap mechanism)
	for (const PreserveBlock& pb : preserve_blocks)
	{
		auto it = m_preservation_map.find(pb);
		if (it == m_preservation_map.end())
			it = m_preservation_map.emplace(pb, std::vector<u8>(pb.length)).first;
		std::memcpy(it->second.data(), EePtr(pb.address), pb.length);
	}

	// Restore memory regions
	for (const BackupLoc& loc : m_backup_locs)
		std::memcpy(EePtr(loc.startAddress), loc.data.data(), loc.endAddress - loc.startAddress);

	// Restore the preserved live ranges on top
	for (const PreserveBlock& pb : preserve_blocks)
		std::memcpy(EePtr(pb.address), m_preservation_map[pb].data(), pb.length);
}
