# Three-fork merge plan (upstream + reliquary + pcsx2x6 + our sdbz)

Measured 2026-09-25 with `git merge-tree` dry runs in this clone (remotes: `upstream` = PCSX2/pcsx2,
`reliquary` = DiscoStarslayer/pcsx2-reliquary, `x6` = PS2Homebrew-arcade/pcsx2x6; push disabled on all three).

## Where each tree stands
| tree | based on upstream | behind upstream | own commits | notes |
|---|---|---|---|---|
| `sdbz` (ours) | v2.7.393, 2026-06-04 | 422 | 8 | Lua script host/bridge, rollback harness, PINE GS reads, scripts |
| `x6/master` (Namco 246/256) | 2026-06-06 | 420 | 217 | never re-synced with upstream (no merges, no cherry-picks) |
| `reliquary/master` | 2026-09-09 | 95 | 222 | Konami Python 1/2, MagicGate/mechacon, iLink, CHD HDD, soft-float recompilers, ParaLLEl-GS, low-latency audio; synced monthly |

Shared work: 6 low-latency-audio commits (DiscoStarslayer) exist in both reliquary and x6 with
identical content -> git merges them without conflict.

## Dry-run conflict counts (files)
| merge | conflicts |
|---|---|
| sdbz + upstream/master | 2 (`Counters.cpp`, `PINE.cpp`) |
| sdbz + reliquary/master | 4 (`.gitignore`, `DisplayWidget.cpp`, `Counters.cpp`, `PINE.cpp`) |
| sdbz + x6/master | 3 (`.gitignore`, `GSRenderer.cpp`, `ImGuiOverlays.cpp`) |
| reliquary + upstream/master | 6 (build scripts, `.sln`, `MemoryCardSettingsWidget.cpp`, `pcsx2/CMakeLists.txt`) |
| x6 + upstream/master | 15 (CI, GameIndex, Audio/Controller settings, DEV9, FullscreenUI, ImGuiManager, InputManager, IopDma, VMManager) |
| reliquary + x6 (raw) | 131 — mostly 4 months of upstream skew; files BOTH forks changed themselves: 87 |

## Recommended structure (keeps updates cheap)
1. `sdbz` stays a small patch stack on top of **upstream** (rebase it; 2 conflicts). This is our
   portable unit: it can be merged into any fork.
2. `x6-sync` = x6/master merged with upstream/master (15 conflicts, once). Offer it back to
   PS2Homebrew-arcade as a PR — if they take it, future syncs are theirs.
3. `integration` = reliquary/master (freshest, synced monthly, has soft-float)
   + merge `x6-sync` (the real work: the 87 files both arcade forks changed — IOP/CDVD/DEV9 hardware,
     input, BIOS/arcade settings, game list, FullscreenUI)
   + merge `sdbz`.
4. Updating later: `git fetch upstream reliquary x6`, then merge upstream -> sdbz, upstream -> x6-sync,
   and reliquary/master + x6-sync + sdbz -> integration. Always **merge** (never rebase) the shared
   branches, and enable `git config rerere.enabled true` so each conflict resolution is recorded and
   replayed automatically next time.

## Order of work
- [x] rebase `sdbz` onto upstream/master -> branch `sdbz-up` (2 conflicts) — pushed
- [x] `x6-sync`: merge upstream into x6 (15 conflicts) — pushed; not built yet
- [x] `integration`: reliquary/master + upstream/master (6 conflicts) + x6-sync (38 files) + sdbz-up (4) — pushed
- [x] build integration (Windows, own deps: `scripts/merge/build_wt.bat integration deps|configure|build`)
- [x] retail boot: FUC runs 3x300 s after the x6 ROM1 fix below; Namco + Python titles still to boot
- [ ] offer `x6-sync` back to PS2Homebrew-arcade (note: gamepad big-picture nav bindings still not wired in x6)
- [ ] port the 8:7 aspect patch, `fuc.lua`

## Branch layout (2026-09-25)
| branch | = | worktree |
|---|---|---|
| `sdbz` | our original stack on v2.7.393 (kept for the old SDBZ builds) | `src` |
| `sdbz-up` | our stack rebased on upstream/master — **the portable unit, new work goes here** | `wt/sdbz-up` |
| `x6-sync` | x6/master + upstream/master | `wt/x6-sync` |
| `integration` | reliquary + upstream + x6-sync + sdbz-up (merge commits only) | `wt/integration` |

Update cycle: `git fetch upstream reliquary x6`; merge upstream into `sdbz-up` and `x6-sync`; in `integration`
merge `upstream/master`, `reliquary/master`, `x6-sync`, `sdbz-up` (that order). rerere replays recorded resolutions.

## Integration resolutions worth knowing
- upstream replaced `PCSX2_qt.sln` with `.slnx`: reliquary's extra projects are re-added by
  `scripts/merge/slnx_add_reliquary.py` (idempotent).
- `GameIndex.yaml`: x6 ships an arcade-only DB; integration = reliquary's full DB + x6 arcade entries
  (`scripts/merge/gameindex_union.py BASE ARCADE OUT`).
- IOP events: `IopEvt_FW` (reliquary) and `IopEvt_SIO2` (x6) both kept, both tested in the rare-interrupt mask.
- Memcard 0xF3 auth reset: reliquary key reload + x6 fix (does not reset the SIO2 terminator).
- `scripts/merge/resolve_hunks.py FILE o|t|ot|to|s,...` resolves conflict hunks per index.
- **x6 ROM1 bug (fixed on x6-sync 57b360c7c):** `vtlb_MapBlock(eeMem->ROM1, 0xB0000000, ...)` wrote past `vtlbdata.pmap`
  (physical map = 512 MB); in integration it corrupted microVU1's program cache -> retail games crashed after ~1-2 min.
  Retail now maps ROM1 at 0x1e000000, arcade leaves it unmapped; vtlb_MapBlock rejects out-of-range ranges in Release.
- Build fixes needed only when combining forks: duplicate `commandByte` in Sio2::Memcard (reliquary hoisted it), two
  `s_pointer_button_state` arrays in InputManager (kept reliquary's atomic one).
- Windows deps: harfbuzz 14.2 / rapidyaml 0.12.1 zips contain symlinks 7-Zip can't create without privilege -> excluded.
- Windows deps: the deps `.bat` must have CRLF line endings when checked out from WSL, or cmd.exe skips lines.
