/*
 * pc_ps2_bridge.h -- the C ABI between an emulator host (the PCSX2 fork, MSVC x64) and PovertyCaster's
 * netcode (pc_ps2bridge.dll, MinGW x64, C++23 inside).
 *
 * WHY A C ABI. PovertyCaster is C++23 built with MinGW; PCSX2 is MSVC. Their C++ runtimes, name mangling
 * and std:: layouts do not mix, so NOTHING C++ crosses this line: plain C structs, fixed-width integers,
 * function pointers, opaque handles. Every struct that crosses carries `struct_size` as its first member
 * so either side can grow it without breaking an older peer (the DLL reads only the prefix it was given
 * and zero-fills the rest; see PCB_ABI_VERSION for the incompatible-change counter).
 *
 * Plain C99. Include from C or C++ on MSVC, MinGW or clang. The full contract (callback sequencing, the
 * deferred plan, replays, what PCSX2 should call when) is targets/ps2bridge/README.md.
 */
#ifndef PC_PS2_BRIDGE_H
#define PC_PS2_BRIDGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#  if defined(PCB_BUILDING_DLL)
#    define PCB_API __declspec(dllexport)
#  else
#    define PCB_API __declspec(dllimport)
#  endif
#  define PCB_CALL __cdecl
#else
#  define PCB_API __attribute__((visibility("default")))
#  define PCB_CALL
#endif

/* Bumped on an INCOMPATIBLE change (a member moved or changed meaning). Growing a struct at its tail is
 * compatible and does NOT bump it -- that is what struct_size is for. pcb_create refuses a mismatch. */
#define PCB_ABI_VERSION 1u

typedef struct pcb_session pcb_session;   /* a netplay / synctest / local session */
typedef struct pcb_replay  pcb_replay;    /* a .pcrep playback */

/* ---- enums (int32_t on the wire; the typedef'd enums are for readability) ------------------------- */

typedef enum pcb_mode {
    PCB_MODE_SYNCTEST = 0,   /* GekkoNet stress session: forced `check_distance`-deep rollback EVERY frame,
                                intra-peer checksum compare. Determinism gate; both slots polled locally. */
    PCB_MODE_LOCAL    = 1,   /* stress session with check_distance 0: plain lockstep, both slots local. */
    PCB_MODE_P2P      = 2    /* GekkoNet game session over UDP (pc::NetChannel), one remote peer. */
} pcb_mode;

typedef enum pcb_dispatch {
    PCB_DISPATCH_IMMEDIATE = 0,  /* the DLL calls host.save/load/advance INSIDE pcb_frame, in GekkoNet order */
    PCB_DISPATCH_DEFERRED  = 1   /* pcb_frame RETURNS the plan; the host executes it and answers every SAVE
                                    with pcb_resolve_save before the next pcb_frame (the PCSX2 shape) */
} pcb_dispatch;

/* The scene, as the netcode needs it. Values are IDENTICAL to pc::NetplayState (static_assert'd in the
 * DLL). Only PCB_SCENE_IN_GAME rolls back; every other scene runs lockstep with an RTT-sized delay (P2P). */
typedef enum pcb_scene {
    PCB_SCENE_UNKNOWN = 0, PCB_SCENE_PRE_INITIAL = 1, PCB_SCENE_INITIAL = 2, PCB_SCENE_AUTO_CHARA_SELECT = 3,
    PCB_SCENE_CHARA_SELECT = 4, PCB_SCENE_LOADING = 5, PCB_SCENE_CHARA_INTRO = 6, PCB_SCENE_SKIPPABLE = 7,
    PCB_SCENE_IN_GAME = 8, PCB_SCENE_RETRY_MENU = 9, PCB_SCENE_REPLAY_MENU = 10
} pcb_scene;

/* Event types. Same codes as the schedule journal's SchedEvent.type (pc/SessionDriver.hpp). */
typedef enum pcb_event_type { PCB_EV_LOAD = 0, PCB_EV_ADVANCE = 1, PCB_EV_SAVE = 2 } pcb_event_type;

typedef enum pcb_log_level { PCB_LOG_INFO = 0, PCB_LOG_WARN = 1, PCB_LOG_ERROR = 2 } pcb_log_level;

/* ---- the plan: what the host must execute for one pcb_frame, IN ORDER ----------------------------- */

typedef struct pcb_event {
    int32_t        type;          /* pcb_event_type */
    int32_t        frame;         /* GekkoNet frame number */
    int32_t        rolling_back;  /* ADVANCE: 1 = re-simulation of an already-run frame (no audio/video) */
    int32_t        save_index;    /* SAVE: the index to pass to pcb_resolve_save; -1 for other types */
    const uint8_t* inputs;        /* ADVANCE: num_players * input_size bytes, slot-major (P1 then P2) */
    uint8_t*       state;         /* SAVE: writable buffer of `state_len` capacity (inline-state hosts copy
                                     their snapshot here); LOAD: the bytes to restore (what that frame's save
                                     left there). Owned by the DLL; valid until the next pcb_frame. */
    uint32_t       state_len;     /* SAVE: capacity (= config.state_size); LOAD: bytes valid */
    uint32_t       flags;         /* PCB_EVF_* */
} pcb_event;

/* LOAD of a replay's state ANCHOR: `state` holds what export_state produced at record time (or, when the
 * recording host had no export_state, a save buffer's bytes). A token host restores it with the same code
 * as its import_state; IMMEDIATE dispatch calls import_state itself when present. */
#define PCB_EVF_ANCHOR_BLOB 1u

typedef struct pcb_plan {
    uint32_t         struct_size;
    int32_t          event_count;
    const pcb_event* events;        /* valid until the next pcb_frame / pcb_replay_frame / destroy */
    int32_t          advances;      /* ADVANCE events (rollback + forward) */
    int32_t          rollback_advances;  /* of those, rolling_back == 1 */
    int32_t          saves;         /* SAVE events = saves the host must resolve (DEFERRED) */
    int32_t          has_load;      /* a LOAD event is present (always first when present) */
} pcb_plan;

/* ---- host callbacks ------------------------------------------------------------------------------- */

typedef struct pcb_desync_info {
    uint32_t struct_size;
    int32_t  frame;
    uint32_t local_checksum;
    uint32_t remote_checksum;   /* SYNCTEST: the checksum the first pass recorded for that frame;
                                   P2P: the remote peer's; REPLAY: the recorded CHECK */
    int32_t  kind;              /* 0 = synctest (resim != forward), 1 = p2p (peers differ), 2 = replay */
} pcb_desync_info;

typedef struct pcb_host {
    uint32_t struct_size;       /* sizeof(pcb_host) */
    void*    user;              /* handed back as the first argument of every callback */

    /* IMMEDIATE dispatch: all three REQUIRED. DEFERRED dispatch: never called (the host runs the plan).
     *
     * save: capture the current game state as `frame` (the state AFTER advancing `frame`). Inline-state
     *   hosts copy up to `cap` bytes into dst and set *out_len; token hosts keep the snapshot in their own
     *   ring and write a small token (or nothing, *out_len = 0 is allowed). Returns the checksum of the
     *   state; 0 is RESERVED and means "no opinion / not compared" -- a real hash that happens to be 0 must
     *   be mapped off it (pcb_finalize_checksum).
     * load: restore the state saved for `frame` (src/len = what that save left in its buffer).
     * advance: run exactly one simulation frame with `inputs`. rolling_back = 1 is a re-simulation of a
     *   frame already shown: do not present video, do not emit audio. */
    uint32_t (PCB_CALL* save)(void* user, int32_t frame, uint8_t* dst, uint32_t cap, uint32_t* out_len);
    void     (PCB_CALL* load)(void* user, int32_t frame, const uint8_t* src, uint32_t len);
    void     (PCB_CALL* advance)(void* user, int32_t frame, const uint8_t* inputs, int32_t rolling_back);

    /* REQUIRED (both modes): this peer's live input for `player` (input_size bytes into out). Called only
     * for the slots this peer owns (both slots in SYNCTEST/LOCAL). */
    void     (PCB_CALL* poll_local_input)(void* user, int32_t player, int32_t frame, uint8_t* out);

    /* OPTIONAL: current pcb_scene. NULL means "always PCB_SCENE_IN_GAME" (rollback everywhere). */
    int32_t  (PCB_CALL* scene)(void* user);
    /* OPTIONAL: one line of diagnostics (no trailing newline guaranteed either way). */
    void     (PCB_CALL* log)(void* user, int32_t level, const char* line);
    /* OPTIONAL: a desync was detected (fires once per session / replay, at the first detection). */
    void     (PCB_CALL* on_desync)(void* user, const pcb_desync_info* info);

    /* OPTIONAL, replay anchors for TOKEN hosts (whose save buffer holds a token, not the state):
     * export_state: serialize the snapshot the host holds for `frame` (just saved) into dst; with dst ==
     *   NULL return the size needed. Returns bytes written (0 = cannot).
     * import_state: restore a blob export_state produced. Returns 1 on success.
     * Without them the anchor is the save buffer's bytes and playback restores it with load(). */
    uint32_t (PCB_CALL* export_state)(void* user, int32_t frame, uint8_t* dst, uint32_t cap);
    int32_t  (PCB_CALL* import_state)(void* user, int32_t frame, const uint8_t* src, uint32_t len);
} pcb_host;

/* ---- session ---------------------------------------------------------------------------------------- */

typedef struct pcb_config {
    uint32_t    struct_size;        /* sizeof(pcb_config) */
    uint32_t    abi_version;        /* PCB_ABI_VERSION */
    int32_t     mode;               /* pcb_mode */
    int32_t     dispatch;           /* pcb_dispatch */
    int32_t     num_players;        /* must be 2 (0 -> 2) */
    int32_t     local_player;       /* P2P: 0 or 1. Ignored otherwise. */
    const char* remote_addr;        /* P2P: "ip:port" of the remote peer (copied) */
    uint16_t    local_port;         /* P2P: UDP port to bind */
    uint8_t     input_delay;        /* local input delay in frames (in-battle delay for P2P) */
    uint8_t     prediction_window;  /* must be 8 (0 -> 8): the rollback depth is FIXED */
    uint8_t     check_distance;     /* SYNCTEST rollback depth; must be 8 (0 -> 8) */
    uint8_t     reserved0[3];
    uint32_t    input_size;         /* bytes per player per frame; 1..16 (0 -> 6 = pcb_ps2_input) */
    uint32_t    state_size;         /* bytes per save slot GekkoNet allocates (inline: the state bound;
                                       token: the token size, e.g. 16). 0 -> 16 */
    const char* replay_path;        /* .pcrep to record this session into; NULL/"" = no recording */
    const char* game_id;            /* <= 15 chars, written into the .pcrep header ("SLUS-21442") */
    int32_t     sched_journal;      /* 1 = arm the rollback-schedule journal (pcb_sched_journal_dump) */
} pcb_config;

/* The default PS2 wire input (input_size = 6): the DualShock2 button word ACTIVE-HIGH (bit set = held; the
 * pad protocol's word inverted) in the pad's own bit order, then the four stick bytes. The host rebuilds
 * the full DS2 pad report (pressure bytes etc.) from this, deterministically, on every peer. Hosts that
 * must carry more set input_size up to 16 and define their own layout. Packed: 6 bytes, no padding. */
typedef struct pcb_ps2_input {
    uint8_t buttons_lo;   /* SELECT L3 R3 START UP RIGHT DOWN LEFT  (bit 0..7)  */
    uint8_t buttons_hi;   /* L2 R2 L1 R1 TRI CIR X SQU              (bit 8..15) */
    uint8_t rx, ry;       /* right stick, 0x80 = centre */
    uint8_t lx, ly;       /* left stick */
} pcb_ps2_input;

typedef struct pcb_stats {
    uint32_t struct_size;
    int32_t  frames_advanced;       /* ADVANCE events dispatched (incl. re-sims) */
    int32_t  current_frame;         /* GekkoNet's next frame to advance */
    int32_t  saves, loads;
    int32_t  rollbacks;             /* ticks that re-simulated >= 1 frame */
    int32_t  last_rollback_frames;  /* depth of the most recent pcb_frame's re-sim (0 = none) */
    int64_t  rollback_frames_total; /* re-simulated frames, cumulative */
    int32_t  desynced;              /* 1 once a desync was detected */
    int32_t  desync_frame;
    uint32_t desync_local_checksum, desync_remote_checksum;
    float    frames_ahead;          /* P2P: >0 = this peer is ahead (should slow down) */
    float    pace_factor;           /* P2P: suggested frame-time multiplier (pc::runtime::riftStretchFactor;
                                       1.0 = nominal, 1.016..1.10 = stretch this frame) */
    uint16_t ping_ms, jitter_ms;
    int32_t  connected_peers;
    int32_t  remote_disconnected;
    int32_t  current_delay;         /* local input delay in effect now */
    int32_t  rollback_mode;         /* 1 = in-battle (rollback) delay in effect, 0 = lockstep delay */
    uint32_t health_compares, health_mismatches;  /* P2P cross-peer checksum compares that ran / failed */
    uint32_t deferred_abandoned;    /* DEFERRED: saves not resolved before the next pcb_frame */
    int32_t  stalled_frames;        /* pcb_frame calls that advanced nothing (the host did not run a frame) */
    int64_t  replay_frames_recorded;  /* advance events the recorder consumed (re-simulations included) */
    int32_t  replay_anchor_frame;   /* -1 until the recording's state anchor was written */
} pcb_stats;

PCB_API uint32_t     PCB_CALL pcb_abi_version(void);
/* NULL on failure; pcb_last_error() says why. Only ONE P2P session may exist per process (the UDP channel
 * is a process singleton); SYNCTEST/LOCAL sessions are unrestricted. */
PCB_API pcb_session* PCB_CALL pcb_create(const pcb_config* config, const pcb_host* host);
PCB_API void         PCB_CALL pcb_destroy(pcb_session* s);   /* finalises the recording, if any */

/* Call ONCE per emulated frame boundary, before the game's sim tick would run.
 * Returns the number of ADVANCE events in the plan (0 = hold: do NOT run a game frame this time), or < 0
 * on error. *out_plan (optional) receives the plan -- in IMMEDIATE mode it has already been executed. */
PCB_API int32_t      PCB_CALL pcb_frame(pcb_session* s, const pcb_plan** out_plan);

/* DEFERRED: answer the plan's SAVE with that save_index. Copy inline state into the event's `state`
 * buffer BEFORE resolving. checksum 0 = abstain. Returns 1 on success, 0 on a bad/duplicate index. */
PCB_API int32_t      PCB_CALL pcb_resolve_save(pcb_session* s, int32_t save_index, uint32_t checksum,
                                               uint32_t state_len);

PCB_API int32_t      PCB_CALL pcb_get_stats(const pcb_session* s, pcb_stats* out);  /* 1 = ok */

/* TEST ONLY: corrupt the checksum this peer reports for `frame` (P2P: the peer must detect it). */
PCB_API void         PCB_CALL pcb_set_desync_inject(pcb_session* s, int32_t frame);

/* Rollback-schedule journal (config.sched_journal = 1): dump it, or re-run one offline through a host
 * (IMMEDIATE callbacks; the journal's base blob is the SAVE BUFFER'S bytes, so this only reproduces
 * inline-state sessions). Returns >= 0 on success (replay: 0 = matched, >0 = first diverging index + 1). */
PCB_API int32_t      PCB_CALL pcb_sched_journal_dump(pcb_session* s, const char* path);
PCB_API int32_t      PCB_CALL pcb_sched_journal_replay(const pcb_config* config, const pcb_host* host,
                                                       const char* path);

/* ---- .pcrep playback ---------------------------------------------------------------------------------
 * Plays round 0 of a recording made by a bridge session: restores the state anchor, then feeds the
 * recorded inputs one frame per call and compares every recorded CHECK against the host's checksum.
 * Same dispatch contract as a session (the plan has LOAD first, then ADVANCE, then SAVE when a CHECK is
 * due). The config supplies dispatch/input_size/state_size (mode etc. are ignored). */
typedef struct pcb_replay_stats {
    uint32_t struct_size;
    int32_t  total_frames;      /* frames of recorded input after the anchor */
    int32_t  frame;             /* next frame to play */
    int32_t  anchor_frame;
    int32_t  checks_total, checks_ok, checks_failed;
    int32_t  first_fail_frame;  /* -1 = none */
    int32_t  finished;
} pcb_replay_stats;

PCB_API pcb_replay*  PCB_CALL pcb_replay_open(const char* path, const pcb_config* config, const pcb_host* host);
PCB_API void         PCB_CALL pcb_replay_close(pcb_replay* r);
/* Returns 1 = a frame was planned, 0 = the recording is finished, < 0 = error. */
PCB_API int32_t      PCB_CALL pcb_replay_frame(pcb_replay* r, const pcb_plan** out_plan);
PCB_API int32_t      PCB_CALL pcb_replay_resolve_save(pcb_replay* r, int32_t save_index, uint32_t checksum,
                                                      uint32_t state_len);
PCB_API int32_t      PCB_CALL pcb_replay_get_stats(const pcb_replay* r, pcb_replay_stats* out);

/* The last error on this thread's most recent failing call ("" when none). */
PCB_API const char*  PCB_CALL pcb_last_error(void);

/* Maps a raw hash off the reserved "no opinion" value 0 (same rule as pc::finalizeChecksum). */
static inline uint32_t pcb_finalize_checksum(uint32_t h) { return h == 0u ? 1u : h; }

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* PC_PS2_BRIDGE_H */
