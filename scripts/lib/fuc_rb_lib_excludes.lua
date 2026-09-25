-- Generated from notes/rollback_lib_excludes.txt: static data accessed ONLY by Sony/CRI/libc code, extent up to the
-- next referenced item (arrays are addressed through their base), plus EE thread stacks (kernel top 0x5094E0, sceMc
-- read-fast 0x514910, CRI ADX x6 0x3B7520..0x3BED28) and the PS2RNA IOP DMA buffer 0x3CC3B8. The newlib reent
-- (0x3B1E00..0x3B2300, libc rand seed 0x3B1F70 read by game code) is deliberately kept tracked.
return {
	{ 0x3B1390, 0xA70 },
	{ 0x3B2628, 0x20DF0 },
	{ 0x4DA450, 0x1BB0 },
	{ 0x4E1E10, 0xF0 },
	{ 0x4E2000, 0x1319 },
	{ 0x4E3420, 0x71D0 },
	{ 0x506FF0, 0x3B0 },
	{ 0x509100, 0x218 },
	{ 0x509380, 0x70F0 },
	{ 0x510B80, 0x7870 },
	{ 0x5239D0, 0x10 },
	{ 0x523A40, 0xB0 },
	{ 0x531C80, 0x2A0 },
	{ 0x531F68, 0x18 },
	{ 0x5361E0, 0x24 },
}
