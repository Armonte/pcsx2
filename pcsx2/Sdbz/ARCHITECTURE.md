# PS2 rollback netplay: architecture plan

Status: plan of record, 2026-09-26. The decisions come from the project owner. The measurements come from the FUC work (notes in the FUC repo, `/mnt/c/dev/ps2/FUC/notes/`).

Games: Fate/Unlimited Codes (FUC, SLPM-55108) and Super Dragon Ball Z (SDBZ, SLUS-21442) are developed in parallel. Kinnikuman Muscle Grand Prix comes next.

Blueprint: Slippi, i.e. game-memory snapshots plus the game re-running its own simulation tick, with no emulator savestates. The platform it plugs into is PovertyCaster (`/mnt/c/dev/castergroup/PovertyCaster`).

## 0. Fixed decisions

| Topic | Decision |
|---|---|
| Platform | The PCSX2 fork becomes a **PovertyCaster host**. It reuses pc-runtime (RuntimeDriver), pc-core SessionDriver (GekkoNet), `.pcrep` replays, pc-spectate, lobby, harness conventions and the `IGameAdapter` seam. |
| Hooks | **Emulator-level EE hooks** (`Sdbz/EeHooks`). No new code caves and no writes to game memory. |
| Game knowledge | A **declarative per-game manifest plus a small C++ adapter**. Lua is the dev/prototyping lane. |
| Iteration | **Hot reload on every layer**: manifest, adapter DLL and Lua. It is a hard requirement. |
| Rollback depth | **Fixed at 8 frames.** Auto-tuning moves only input delay and pacing. |
| Audio | **Intent reconcile.** Live audio is never rolled back. Anything the simulation reads about audio is virtualized into rolled-back memory. |
| Emulation settings | **A locked profile per game**, hashed and compared at connect. |
| Beyond Slippi | Stat-rich replays, replay takeover and training, spectate with desync forensics, auto-tuned delay. |
| OS | Windows x64 first. |

## 1. Layers

```
 povertycaster.exe (launcher, lobby, rendezvous)          -- unchanged, gains a "PS2" game family
        | env / pc-ipc
 pcsx2-qt.exe (our fork)  == PovertyCaster host "pcsx2host"
   +-- pc-runtime RuntimeDriver + pc-core SessionDriver(GekkoNet) + pc-replay + pc-spectate   (linked in, x64)
   +-- Ps2HostAdapter : IGameAdapter        generic PS2 base adapter (this repo)
   |     +-- StateEngine    = RollbackDevice + PageSnapshotRing   (capture/load/hash, 8f ring, resim control)
   |     +-- EeHooks        = recompiler/interpreter hooks        (gates, calls, call-site skip/replace, probes)
   |     +-- InputFeed      = PadFeed                             (raw DS2 report injection at the game's pad read)
   |     +-- AudioIntent    = sound request journal + reconcile   (per-game rules from the manifest)
   |     +-- EeThunks       = host-assembled EE glue in reserved EE RAM (resim step loop, call trampolines)
   |     +-- SettingsLock   = per-game emulation profile + hash
   +-- GameManifest (TOML, hot-reloaded)  +  GameAdapter DLL (C ABI, hot-reloaded)   one pair per game
   +-- Lua ScriptHost (dev UI, prototyping: can register the same hooks, hot reload)
```

### 1.1 How it plugs into PovertyCaster (from the survey of its code)

The survey found that none of RuntimeDriver, SessionDriver, replay, spectate or parity depends on a particular game. They reach the game only through `IGameAdapter` (`pc-core/include/pc/adapter/IGameAdapter.hpp`) and the `extern "C" pcrt_*` host hooks. PCSX2 becomes a **new target** next to `targets/pchost`. It is not an injected DLL.

- **Frame driver.** PCSX2 has no game function to detour, so the vsync boundary calls `RuntimeDriver::onFrame()`. `pcrt_runGameFrame` means "let the EE run to the next frame boundary". PCSX2's limiter is off while RuntimeDriver paces (`pcrt_takeOverFrameClock`).
- **`advance(inputs, frame, rollingBack)`.**
  - Forward pass: PadFeed injects the inputs and the EE runs one normal frame.
  - Rollback: the resimulation runs *inside* the frame, driven by the game itself (Slippi `LoopEngineForRollback`, today's FUC cave loop), through EeThunks.
  - SessionDriver's load, then advance×N, maps onto one StateEngine rollback of N resim steps. The adapter batches them.
- **`save`/`load`/checksum.**
  - The page ring provides save and load.
  - The checksum is a masked hash of the manifest's hashed regions. **Capture and hash come from one list** (PovertyCaster's rule): excluded means neither captured nor hashed.
  - `SaveRegion` addresses are `eeMem` offsets.
  - `loadForeign` (deep join, journals) takes an encoded blob of the ring pages.
- **Input.** `writeGameInput` goes to PadFeed, never into EE RAM directly, so the game's own pad decode (edges, repeat) runs unchanged.
- **Scene and phases.** `sceneState` / `roundDecided` / `simTickBlocked` / attach timing come from **manifest predicates**. These are EE reads such as FUC `g_GameWork_Flags`; the full phase map is in `notes/BATTLE_PHASES.md`. Only fighting frames roll back. Every other phase runs lockstep or feeds neutral input (`setFeedNeutral`).
- **RNG attach.** `IRngSync` covers the manifest's RNG words (FUC: `g_RandSeed` plus the split render/sound streams).
- **Overlay/video.** PCSX2's ImGui replaces pc-overlay's D3D hooks, and `IVideoControl` targets the PCSX2 window.
- **Gaps we add to PovertyCaster.**
  - Adapter hot reload: its adapters are statically linked today.
  - A shared audio seam: `IAudioRollback` was deleted, and only `RollbackSfxJournal` is shared. Our AudioIntent could become that shared seam.

### 1.2 EeHooks (implemented: resim gate plus C++ call)

- A hooked address always starts its own recompiled block. The hook is emitted at the block entry, where no guest register is cached on the host, so the hook can read and write `cpuRegs` directly.
- The interpreter runs the same hooks.
- Adding or removing a hook drops the affected compiled blocks on the CPU thread, which is what makes hooks hot-reloadable.

| Kind | Status | Emitted code | Use |
|---|---|---|---|
| `ResimGate` | done | `cmp byte [resim],0; jz run; pc=ra; exit` | void work skipped in re-simulated frames (draw leaves, voice control) |
| `Call` | done | flush-free `call handler(pc)`; handler may return | cold-path logic (per frame or per event) |
| `CallSiteSkip` | next | at a `jal` site: skip the call (delay slot kept) while resim | FUC S1–S5/C1 gates |
| `CallSiteReplace` | next | at a `jal` site: call a host handler instead, with args/return in regs | sound-only RNG draws, pad read feed |
| `Probe` | next | handler logs {frame, pass, a0..a3, ra}, no semantics change | sound request journal, forensics |

**Performance (proved 2026-09-26).**

- Setup: FUC, 8f sync test on every frame. Six function-entry gates were moved from cave trampolines to native gates, and a native counter confirmed ~64 gated calls per re-simulated frame.
- Result: 0 desync.
- The resim cost per rollback matches within noise over 12 interleaved pairs: median 6.78 vs 6.62 ms, then 6.96 vs 7.21 ms with the counter adding cost to the hook side only, and a paired median of −0.1 ms.
- Hooks cost well under 1% of a rollback either way. The emitted path is also structurally shorter: one compare against three extra EE blocks.
- Results: `FUC/states/runs/ab_emugates_*.txt`.
- **Rule:** hot paths (hundreds of calls per frame) use native kinds (gate, skip). C++ `Call` is for cold paths.

### 1.3 EeThunks: the one piece of EE code that must exist

The resim loop has to execute game functions (the sim tick, event pass, per-frame bookkeeping) N times inside one frame, and the EE runs only EE code. Hand-written caves in dead game code are replaced by:

- **A reserved EE RAM block**, proven unused. It comes from one of two sources:
  - the game heap at boot, via an init hook, so it lives inside rolled-back memory;
  - a RAM range that the dirty-page census and the code-block coverage logger prove the game never touches.
- **The host assembles the thunks from the manifest**:
  - `resim_step = [Task_RunMainListNoArg, {Task_RunMainList, a0=8}, _rwFrameSyncDirty, ...]` becomes a generic loop that calls each step, then makes the device syscall;
  - the hook on the sim-tick call site redirects into it.
- **No per-game assembly.** The step list is data. The assembler is the one in `FUC/tools/rollback_cave.py`, ported to C++.
- **Space is effectively unlimited** (KBs available). FUC's cave had 1,824 B, with about 43 free words at the end.

### 1.4 GameManifest (TOML, hot-reloaded at the next frame boundary)

```toml
[game]         serial="SLPM-55108" crc=... name="Fate/Unlimited Codes"
[settings]     ee_softfloat=true  ee_round="chop"  vu0_softfloat=false  mtvu=false  gamefixes=[...]   # hashed at connect
[frame]        sim_tick_site=0x159F98   pad_read_site=0x2103EC   frame_counter=0x51D8DC
[resim]        steps=["0x211260", {fn="0x211130", a0=8}, "0x2EB2A0", "0x1F18A0", "se_dup_age", "tint_per_player", "0x2190F0"]
[state]        regions=[[0x3B1080,0x536280],[0x5362C0,0x1FBD000]]
               exclude=[ ... one list: never captured, never hashed ... ]      # generated by tools/gen_rb_excludes.py
               resim_restore=[ ... audio statics ... ]
               dynamic=[ {chain=[0x51C870,"+4"], stride=8, count=10, field=[0x24D4,0x14], kind="exclude+resim_restore"},
                         {chain=[0x51A420,"+48"], stride=56, count=4, len=[143844,71972,71972,143844], kind="exclude"} ]
[rng]          sim=0x522D80  split_render=true  sound_sites=[...]
[hooks]        gate=[0x32D3C0, 0x217920, ...]   skip_site=[0x1AB950, ...]   replace_site=[...]
[phases]       fighting="u32[0x51D8A0] & 2"   input_inert="..."  round_decided="..."
[audio]        stream_status_ops=[...]   virtual_clock=true   announcer_table="derived:adx_headers"
[replay.stats] p1_hp="u32[[0x51C874]+0x23C]"  ...                          # per-frame extraction for stat replays
```

- **Generated, then checked.** The RE tools (IDA accessor sweeps, audio census, dynamic-exclude discovery) *emit* manifest sections, and the harness *checks* them: sync test, probe-diff, coverage. Hand-edited lines carry a `# why` comment.
- **Per-game C++ adapter DLL** (stable C ABI `ps2_adapter_v1`: a function table, no global state; all state lives in EE memory or the host). It is only for what data can't express: phase logic with side effects, virtual audio clock rules, stat extractors, menu drivers. It is rebuilt and swapped with the game running. The host unloads it at a frame boundary, drops its hooks, loads the new DLL and re-registers.
- **Lua** can register every hook kind and read/write the manifest in memory, for fast prototyping. Anything proven gets promoted into the manifest or the adapter.

## 2. Audio: intent reconcile

This is PovertyCaster's model, adapted to the PS2. It is proven in pieces on FUC.

1. **Live playback is never rolled back.** That covers SPU2/IOP, the game's sound driver statics, and **middleware buffers in the heap**. FUC's CRI ADX work areas sit in `g_SysHeap`, and rolling them back caused the replays heard on 2026-09-26. The same list serves capture and hash.
2. **Re-simulated frames are invisible to audio.**
   - Audio statics are saved at rollback start and restored at resim end.
   - Nothing issued during resim reaches the IOP. Confirmed on FUC with a live census: 0 RPCs, 0 queue plays, 0 ADX stop/pause during resim.
3. **The simulation never reads live audio.** Any "is it playing / did it get a slot" query is answered from a virtual model kept in rolled-back memory:
   - FUC announcer waits: `ScrOp_1C` 09/0A/0B/0C/0D/0E/10/48/49, backed by stream start frame plus length in frames, taken from the ADX headers;
   - SE handles are deterministic virtual handles.
4. **Reconcile after a rollback.** Sounds started inside the mispredicted window that are absent from the corrected timeline are cut. A sound that should be playing but is silent is never restarted mid-sound. The request journal comes from `Probe` hooks, and the tooling is shared with pc-core `RollbackSfxJournal`.
5. **Proof tooling** (built): an ordered probe log per run plus a forward-vs-rollback diff (`fuc_test_match.sh --trace`, `R=0` baseline).

## 3. Netplay behaviour

- **GekkoNet through SessionDriver**:
  - prediction window 8, `check_distance` 8, desync detection on;
  - one session per match;
  - lockstep with an RTT-sized latched delay outside fighting phases; rollback only while fighting.
- **Fixed 8f depth.** Auto-tune picks input delay 0..N from measured RTT and jitter plus the game's measured rollback cost, and re-tunes between rounds.
- **Time sources pinned.** The KO slow-motion length on FUC is driven by the vblank delta at `0x522E10`, so it is pinned to 1. Any host-time or IOP-timing read the simulation makes goes into the manifest as a virtualized value.
- **Settings lock hash** at connect, together with the ISO CRC and the manifest hash.

## 4. Beyond Slippi

| Feature | Build on |
|---|---|
| Stat-rich replays | `.pcrep` input tapes plus manifest `[replay.stats]` extraction every frame (HP, meter, positions, action ids, combo counter). Periodic ring snapshots are stored so seeking is instant. |
| Replay takeover / training | Seek to any frame (snapshot plus re-sim), then switch a side from tape to live pad or the CPU AI (FUC: `player+0x2440`, `notes/CPU_AI_INPUT.md`). Export any situation as a practice start. |
| Spectate plus desync forensics | pc-spectate stream. On a checksum mismatch both peers dump the ring pages of the diverging frame, the probe log and the input tape. The desync-sniffer flow attributes the byte to its writer in IDA. |
| Auto-tuned delay | SessionDriver health stats plus the per-game rollback cost measured by StateEngine. |

## 5. Performance budget (8f, 60 fps)

Measured on FUC, quiet host, sync test on every frame, which is harsher than netplay:

| Item | Cost |
|---|---|
| 8f rollback | 5.7 ms: load 0.28, resim 4.3, captures 1.1 |
| Present pacing | avg 16.73 ms |

The levers that got there: host-vsync skip, counter park, IOP skip during resim, visual-only resim gates, incremental page ring with parallel copy, a snapshot pool, and hot-page promotion.

Every new mechanism is A/B-tested against the previous one (interleaved pairs, medians) before it replaces it. A regression larger than the noise blocks the change.

## 6. Milestones (FUC and SDBZ in parallel)

1. **M1 hooks complete:**
   - `CallSiteSkip`, `CallSiteReplace`, `Probe`, EeThunks, and the code-block coverage logger;
   - FUC runs with **zero caves**, with identical sync test, probe-diff and A/B results.
2. **M2 manifest:**
   - TOML loader with file-watch hot reload;
   - FUC's `fuc.lua` rollback config moved into `fuc.toml`, generated by the tools;
   - an SDBZ manifest started from `sdbz/SDBZ_ROLLBACK_PLAN.md` plus its RE notes;
   - Lua becomes UI only.
3. **M3 adapter ABI:**
   - `ps2_adapter_v1` with DLL hot swap;
   - Ps2HostAdapter implements `IGameAdapter` against StateEngine;
   - GekkoNet stress session (synctest) through SessionDriver on both games.
4. **M4 audio:**
   - FUC virtual announcer clock and KO slow-mo pin;
   - the reconcile pass;
   - the SDBZ sound census, using the same method and probes.
5. **M5 netplay:**
   - the PovertyCaster launcher starts pcsx2host;
   - two-machine local test, `.pcrep` record and playback, schedule-journal replay offline.
6. **M6 beyond Slippi:** stat replays, takeover, forensics dumps, auto-tune.
7. **M7 Kinnikuman:** manifest-first bring-up, the proof that a third game is mostly data.

## 7. Open questions

- The EeThunks home: heap allocation at boot (rolled back, simplest) or a proven-free RAM range. Decided per game by coverage and census.
- Does PovertyCaster's pc-core/pc-runtime compile cleanly for x64 MSVC inside the PCSX2 build? The survey says the core is meant to build for both architectures; this still needs verifying.
- Cross-peer determinism of the recompiler: the same build is required, which the settings lock enforces. Should CI run a Windows-to-Windows determinism check across CPU vendors (Intel/AMD)?
