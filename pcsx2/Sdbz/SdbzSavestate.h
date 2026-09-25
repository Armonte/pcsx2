// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <map>
#include <vector>

// Direct structural port of Project Slippi's SlippiSavestate (refs/Ishiiruka:
// Source/Core/Core/Slippi/SlippiSavestate.{h,cpp}) for SDBZ (SLUS-21442).
//
// A "savestate" here is ONLY the game's mutable EE memory (data + BSS + heap), captured and
// restored with plain memcpys. No CPU or emulator state is included: capture and load always
// happen at the same EE frame-boundary hook (Counters VSyncStart), so the execution context is
// identical by construction -- the same trick Slippi uses (their EXI device memcpys GC RAM
// regions at a fixed game-side hook; registers never need saving).
//
// The region list is [dataStart, heapEnd) minus exclude sections (state that must keep running
// linearly and never roll back: async IO, sound mirrors, ...). The exclude list is discovered
// empirically by the SdbzDeterminism harness -- Slippi's was hunted the same way.
class SdbzSavestate
{
public:
	struct PreserveBlock
	{
		u32 address = 0;
		u32 length = 0;
		bool operator<(const PreserveBlock& o) const
		{
			return (address != o.address) ? (address < o.address) : (length < o.length);
		}
	};

	struct BackupLoc
	{
		u32 startAddress = 0; // EE physical address (masked into eeMem->Main)
		u32 endAddress = 0;   // exclusive
		std::vector<u8> data;
	};

	// Builds the region list immediately (heapEnd must already be resolved by the caller --
	// e.g. read live from the game's own malloc high-water mark, g_HeapHighWaterEnd@0x500708).
	SdbzSavestate(u32 dataStart, u32 heapEnd, std::vector<PreserveBlock> excludes);

	// memcpy EE regions -> host buffers. EE/CPU thread only.
	void Capture();

	// memcpy host buffers -> EE regions, keeping `preserve_blocks` (caller-owned live ranges,
	// same mechanism Slippi uses for e.g. text buffers). EE/CPU thread only.
	void Load(const std::vector<PreserveBlock>& preserve_blocks);

	const std::vector<BackupLoc>& Regions() const { return m_backup_locs; }
	u32 TotalBytes() const;

private:
	void initBackupLocs(u32 dataStart, u32 heapEnd, std::vector<PreserveBlock> excludes);

	std::vector<BackupLoc> m_backup_locs;
	std::map<PreserveBlock, std::vector<u8>> m_preservation_map;
};
