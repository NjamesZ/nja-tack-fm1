/* SPDX-License-Identifier: GPL-3.0-only
 * TACK for FM-1 — bootable TACK surface + fixed one-bar-per-chord band.
 * The four tracks are preloaded with a TACK four-chord loop and the sequencer
 * starts automatically. Style changes remap BPM, dens, engines, kits, sends,
 * global FX (delay/reverb/chorus), comp/bass rhythms and drum hit patterns from
 * the browser MOODS table; drums evolve every bar. */
#include "tack/tack.h"
#include "tack/tack_phrases_user.h"
#include "tack/tack_rec_policy.h"

static TackState tack_state;
static uint8_t tack_ready;
static uint8_t tack_sel;
static uint8_t tack_pick_slot; /* 6.2: keys 1–6 apply to this slot (current/first), then advance */
static uint32_t tack_k2_ms, tack_k3_ms, tack_k4_ms;
static uint8_t tack_move_rate, tack_move_variant, tack_move_phase, tack_last_bar;
static uint8_t tack_pending;
static uint8_t tack_auto;
static uint8_t tack_user_chose; /* 1 = user picked/captured a start chord for Auto */
static TackChord tack_user_chord; /* last manual chord — Auto cycle may begin here */
static uint8_t tack_hold;
static uint8_t tack_hold_bar;
/* Hand-chord phrase recorder: free-capture while REC (NOT seq-coupled).
 * Stop → HELD as played. LOOP / APPLY / QNT / RHY / mode = opt-in only. */
#define TACK_CREC_MAX 16u
static uint8_t tack_crec_mode;   /* 0=off/HELD 1=REC 2=LOOP (opt-in) */
static uint8_t tack_crec_len;
/* 6.2 LOOP→Mac export snapshot (SysEx 82 / tools/tack_export_phrase.py) */
static uint8_t tack_ex_len;
static uint8_t tack_ex_rhy;
static uint8_t tack_ex_root;
static uint8_t tack_ex_style;
static uint16_t tack_ex_tempo;
static TackChord tack_ex[TACK_CREC_MAX];
static uint8_t tack_ex_armed; /* 1 = snapshot ready for host pull */
static uint8_t tack_crec_pos;
static TackChord tack_crec[TACK_CREC_MAX];
static uint32_t tack_crec_ms[TACK_CREC_MAX]; /* ms since REC start — free timing */
static uint8_t tack_crec_step[TACK_CREC_MAX]; /* filled only on APPLY/QNT/RHY */
static uint8_t tack_crec_steps_ready; /* 1 = steps derived from ms (post-OK) */
static uint32_t tack_crec_t0;     /* fm1_ms at REC arm */
static uint32_t tack_crec_loop_t0; /* fm1_ms at LOOP start */
static uint32_t tack_crec_span_ms; /* phrase span for free LOOP wrap */
static uint32_t tack_crec_down_ms; /* press start for long-clear; 0=idle */
static uint8_t tack_crec_long_done;
static uint8_t tack_crec_last_fire; /* last crec index fired in LOOP */
/* 6.28-rc1 MIDI phrase extras (parallel to token captures from keys/moves). */
static TackRecSession tack_midi_sess;
static uint8_t tack_crec_src[TACK_CREC_MAX]; /* 0=harmony token, 1=MIDI classified */
static uint8_t tack_crec_kind[TACK_CREC_MAX];
static uint8_t tack_crec_lit_n[TACK_CREC_MAX];
static uint8_t tack_crec_lit[TACK_CREC_MAX][TACK_REC_MAX_VOICES];
static uint8_t tack_bar_lit_n[4];
static uint8_t tack_bar_lit_kind[4];
static uint8_t tack_bar_lit[4][4];
static uint8_t tack_slot_owned[TACK_SLOTS]; /* 1 = user-captured/applied; survive rebuild */
/* Performed MIDI LOOP (comp lane): immutable while looping; one main-loop
 * clip producer; audio owns its private copy and all gates. Per raw note:
 * channel/pitch identity, onset/off, frozen mapped off. */
static TackNoteEv tack_loop_raw[TACK_REC_MAX_NOTES];
static uint8_t tack_loop_ch[TACK_REC_MAX_NOTES], tack_loop_kind[TACK_REC_MAX_NOTES];
static uint8_t tack_loop_active[TACK_REC_MAX_NOTES], tack_loop_fired[TACK_REC_MAX_NOTES];
static uint8_t tack_loop_mapped[TACK_REC_MAX_NOTES];
static uint8_t tack_loop_raw_n;
static uint8_t tack_rec_failed;
static uint32_t tack_loop_cycle, tack_loop_last_elapsed;
static volatile uint32_t tack_loop_request;
static uint32_t tack_loop_seen;
static TackNoteEv tack_audio_raw[TACK_REC_MAX_NOTES];
static uint8_t tack_audio_kind[TACK_REC_MAX_NOTES], tack_audio_n;
static uint32_t tack_audio_t0, tack_audio_span;
static void tack_midi_audio_tick(void);

static uint8_t tack_loop_tick_valid;
static void tack_midi_loop_stop(void);
static void tack_midi_loop_tick(void);
static void tack_midi_rec_drain(void);
static void tack_midi_rec_reset(void);
static int tack_midi_loop_held(const track_t *, uint32_t);
typedef struct {
    TackChord identity;
    uint8_t owned, n, kind, notes[4];
} TackSlotMeta;
static TackSlotMeta tack_phrase_meta[2][TACK_SLOTS];
static TackSlotMeta tack_undo_meta[4][TACK_SLOTS];
static void tack_meta_store(TackSlotMeta out[TACK_SLOTS])
{
    uint8_t i, k;
    for (i = 0; i < TACK_SLOTS; i++) {
        out[i].identity = tack_state.chords[i];
        out[i].owned = tack_slot_owned[i]; out[i].n = tack_bar_lit_n[i];
        out[i].kind = tack_bar_lit_kind[i];
        for (k = 0; k < 4u; k++) out[i].notes[k] = tack_bar_lit[i][k];
    }
}
static void tack_meta_load(const TackSlotMeta in[TACK_SLOTS])
{
    uint8_t i, k;
    for (i = 0; i < TACK_SLOTS; i++) {
        tack_slot_owned[i] = 0; tack_bar_lit_n[i] = 0;
        if (in[i].identity.root != tack_state.chords[i].root
            || in[i].identity.quality != tack_state.chords[i].quality) continue;
        tack_slot_owned[i] = in[i].owned;
        tack_bar_lit_n[i] = in[i].n; tack_bar_lit_kind[i] = in[i].kind;
        for (k = 0; k < 4u; k++) tack_bar_lit[i][k] = in[i].notes[k];
    }
}


static uint8_t tack_kb_quality(const uint8_t *n, uint8_t cnt);
static void tack_fill_scale(TackScale *sc);
static void tack_bar_apply_literal(uint8_t bar, uint8_t idx);
void tack_midi_cc(uint8_t cc, uint8_t val);
void tack_midi_note(uint8_t channel, uint8_t note, uint8_t vel);
void tack_midi_self(uint8_t note, uint8_t vel);
static uint8_t tack_auto_last_step; /* Auto grid fire guard */
static uint8_t tack_crec_qnt;    /* 0=off 1=on — Quantize opt-in after REC OK */
static uint8_t tack_loop_sel;   /* LOOP page: 0 REC 1 LOOP 2 QUANTIZE 3 RHYTHM */
static uint8_t tack_crec_rhy;   /* rhythm template — applied only when user asks */
/* Auto/living chord grid: next change lands on this step-in-bar (not always 0). */
static uint8_t tack_chord_grid;      /* 0..15 target step for next Auto move */
static uint8_t tack_chord_grid_armed; /* 1 = Auto change pending at grid step */
static uint8_t tack_style_idx;
static int8_t tack_octave; /* -2..+2 harmonic octave (comp/bass/air); drums untouched */
static uint8_t tack_fill;
static uint8_t tack_dens;          /* MOODS density 0..100; drives hat/perc amount */
static uint8_t tack_loop;          /* phrase loop count for deterministic bar variation */
static uint32_t tack_last_notes;
static char tack_action[24];
static uint32_t tack_action_until;
static uint32_t tack_action_serial;
static uint32_t tack_sig;
static uint8_t tack_drawn;
static uint32_t tack_boot_until; /* 0=done; else expire at fm1_ms — OLED TACK splash */
static uint8_t tack_boot_painted; /* 1 = splash black fill done (one-shot) */
static uint8_t tack_mode_idx;
static uint8_t tack_mute;
static uint8_t tack_solo;     /* 6.2 soft solo bitmask — if any set, only those lanes audible */
static uint8_t tack_mute_sel;
static int16_t tack_level[4];
enum {
    TACK_PG_HOME = 0,
    TACK_PG_SOUNDS,   /* EDIT — kits + ambient worlds */
    TACK_PG_STYLE,    /* SCL  — style room + master BPM */
    TACK_PG_ALGO,     /* LFO  — engine / algorithm */
    TACK_PG_PRESET,   /* from ALGO via K2, or quick */
    TACK_PG_FX,       /* FX   — delay / reverb / chorus / shimmer */
    TACK_PG_FEEL,     /* long ENV — dens / fill / roll / undo / clk / lvl / pan */
    TACK_PG_DRUMS,    /* GLO  — 6.10 PART circle grid (blit); FEEL via GLO cycle */
    TACK_PG_GEN,      /* ENV short tab — Generative: Auto / Chaos / dens / morph / freeze / variation */
    TACK_PG_LOOP,     /* REC short tab — chord phrase looper: Rec / Quantize / Chords / Rhythm */
    TACK_PG_LEARN,    /* 5.8 MIDI learn — arm CC → target map */
    TACK_PG_ZEN,      /* 5.8 one-knob calm */
    TACK_PG_COUNT
};
static uint8_t tack_page;
static uint8_t tack_snd_bank;   /* 0 kits..8 industrial, 9 juno */
static uint8_t tack_snd_cursor; /* Sounds library browse highlight */
static uint8_t tack_snd_depth;  /* 0=LIBRARIES list, 1=sounds inside library (EDIT two-step) */
static uint8_t tack_snd_pick;   /* sound index within active/cursor library (browse cursor) */
static uint8_t tack_trk_lib_pick[4]; /* 6.4: library catalog index assigned per track */
static uint32_t tack_k1_ms;     /* debounce K1 bank/style browse */
static uint8_t tack_chaos;      /* 0 off — nothing random unless user enables */
static uint8_t tack_pong;       /* ping-pong-ish delay: alternate DCOLOR + wider mix */
static uint8_t tack_shimmer;    /* air shimmer amount 0..3 */
static uint8_t tack_air_freeze; /* 1 = lock air (chorus/shimmer/rev) — no style rewrite */
static uint8_t tack_air_morph;  /* 0..3 morph depth when style/scene changes */
static uint8_t tack_morph_left; /* bars remaining to ease air toward target */
static uint8_t tack_air_target; /* target chorus depth while morphing */
static uint8_t tack_style_cursor; /* Style page browse index */
static uint8_t tack_fx_sel;     /* FX page row 0..13 (… + FLANGER/BITRED/VCHO/CHAOS/TAPE + drum + air) */
static uint8_t tack_feel_sel;   /* Feel page row */
static uint8_t tack_gen_sel;    /* GEN page row */
static uint32_t tack_env_down_ms; /* ENV short=GEN tab / long=FEEL */
static uint8_t tack_env_long_done;
static uint8_t tack_drm_sel;    /* Drums FEEL row 0..9 (dens..comp + RAND DRM) */
static uint8_t tack_drm_depth;  /* 0=PART step grid, 1=FEEL dens/hats/... */
static uint8_t tack_drm_lane;   /* PART: voice/lane 0..7 */
static uint8_t tack_drm_step;   /* PART: step cursor 0..bar_steps-1 */
static uint8_t tack_drm_hats;   /* 0=quiet 1=norm 2=more — hat amount feel */
static uint8_t tack_drm_spare;  /* 1 = thinner dens / more air */
static uint8_t tack_kick_lock;  /* 1 = downbeat kicks only (skips Jazz/Jungle/Reggae) */
static uint8_t tack_beat_kit;   /* 0=style 1=CR-78 beat 2=707 beat (Sounds/Drums pick) */
static uint8_t tack_kit_sel;    /* 0..3 classic kit held by user; 0xFF = style owns drums */
static uint8_t tack_arp_last;   /* last non-OFF P_AMODE (default UP=1); style/lib/kit never touch */
/* TACK 6.28-rc3 / FM-1_991 — Soft library readable: AF_M rows, no jump, no fat LIVE bar, one tip; tempo/Chaos/FX/Zen keep. */
static uint8_t tack_drm_ratchet; /* 0=off 1=×2 2=×3 3=×4 hat/fill ratchets */
static uint8_t tack_drm_part_own; /* 1 = user PART owns drum hits; patterns/style flush skip trk[3] rewrite */
static uint8_t tack_rand_drm; /* 6.19: 1 = GEN last fire owns PART (was RAND DRM) */
static uint8_t tack_drm_lock;  /* 6.19: lane lock bitmask — GEN/REMIX/DENSITY skip locked lanes */
static uint8_t tack_air_user; /* 1 = user library pick on air (trk[2]); style must not stomp sound */
static uint8_t tack_fx_scene;    /* 0 Dry 1 Room 2 Dub 3 Shimmer */
static uint8_t tack_fx_scene_lock; /* 1 = scene survives style apply */
static int8_t  tack_lane_pan[4]; /* Comp/Bass/Air/Drums pan −64..63 */
static uint32_t tack_play_down_ms; /* PLAY hold → Panic */
static uint8_t tack_play_long_done;
/* Bar-end / cycle-quantize queues (0xFF = idle). Apply at bar wrap while playing. */
static uint8_t tack_q_style;  /* pending style idx or 0xFF */
static uint8_t tack_q_snd;    /* pending sound bank or 0xFF */
static uint8_t tack_q_kit;    /* pending classic kit 0..3 or 0xFF */
static uint8_t tack_q_fxsc;   /* pending FX scene or 0xFF */
/* A/B arrange — phrase A/B × short section columns (NOT stock 8×8 / NOT 8-part). */
#define TACK_ARR_MAX 8u
static TackChord tack_phrase[2][TACK_SLOTS]; /* 0=A 1=B */
static uint8_t tack_ab_live;     /* 0=A 1=B — live phrase under edit + play */
static uint8_t tack_arr[TACK_ARR_MAX]; /* section columns: 0=A 1=B */
static uint8_t tack_arr_len;     /* 4 or 8 columns in the strip */
static uint8_t tack_arr_pos;     /* playhead section while arrange is ON */
static uint8_t tack_arr_on;      /* 1 = strip advances phrases at loop wrap */
static uint8_t tack_arr_edit;    /* 1 = Home strip edit (K1 col / K2 A↔B) */
static uint8_t tack_arr_sel;     /* column cursor in arr-edit */
static uint32_t tack_k3_down_ms; /* Home K3 long = BRIDGE; short = A↔B */
static uint8_t tack_k3_long_done;
static uint32_t tack_home_down_ms; /* long HOME = arr-edit toggle */
static uint8_t tack_home_long_done;
/* TACK 5.3 — named performances P1..P8 + setlist strip + show lock */
#define TACK_PERF_N 8u
typedef struct {
    uint8_t used;       /* 1 = slot has a snapshot */
    uint8_t style;
    uint8_t snd_bank;
    uint8_t amb_tex;
    uint8_t fx_scene;
    uint8_t ab_live;
    uint8_t arr[TACK_ARR_MAX];
    uint8_t arr_len;
    uint8_t arr_on;
    uint8_t dens;
    uint8_t beat_kit;
    uint8_t kit;        /* 0..3 classic kit index */
    uint8_t drm_dist;
    uint8_t drm_comp;
    int8_t  lane_pan[4];
    uint8_t level_base[4];
    TackChord phrase[2][TACK_SLOTS]; /* A/B arrange only — never clears REC phrase */
} TackPerf;
static TackPerf tack_perf[TACK_PERF_N];
static uint8_t tack_set_sel;   /* setlist cursor 0..7 (= which Performance) */
static uint8_t tack_set_live;  /* last recalled / sounding slot */
static uint8_t tack_q_perf;    /* pending performance idx or 0xFF */
static uint8_t tack_show_lock; /* explicit LOCK only (long OCT+); never auto on PLAY */
static uint32_t tack_save_down_ms;
static uint8_t tack_save_long_done;
static uint32_t tack_k4_lock_down_ms;
static uint8_t tack_k4_lock_done;
static uint8_t tack_set_browse; /* 1 = SAVE held: K1 browses setlist without queue */
static uint32_t tack_more_until;
/* 5.8 MIDI learn + Zen */
static uint8_t  tack_zen_mode;
static uint8_t  tack_zen_prev_pg;
static uint8_t  tack_zen_calm;          /* 0..127 one-knob */
static uint8_t  tack_learn_armed;
static uint8_t  tack_learn_target;      /* index into learn target table */
static uint8_t  tack_learn_last_cc;
static uint8_t  tack_learn_last_val;
static uint8_t  tack_learn_map[16];     /* target → CC# ; 0xFF = unset */
static uint8_t  tack_user_phrase_sel;
 /* Home MORE chrome (SET+hints) until this fm1_ms */
static void tack_nudge_more(void);

/* 5.7 color accents for clarity — fixed RGB hierarchy; soft dirty-region only (no blink). */
#define TACK_COL_PLAY  RGB(255, 160, 60)   /* warm orange PLAY */
#define TACK_COL_A     RGB(120, 180, 255)  /* soft blue A */
#define TACK_COL_B     RGB(255, 180, 80)   /* soft amber B */
#define TACK_COL_SEL   RGB(90, 200, 255)   /* strong selection */
#define TACK_COL_LIVE  RGB(60, 200, 100)   /* 6.23 fixed LIVE + list sel green */
#define TACK_COL_GEN   RGB(160, 120, 255)  /* violet GEN */
#define TACK_COL_LOOP  RGB(255, 100, 120)  /* coral LOOP */
#define TACK_COL_LEARN RGB(80, 220, 200)   /* mint LEARN */
#define TACK_COL_ZEN   RGB(180, 140, 255)  /* soft violet ZEN */
#define TACK_COL_STYLE RGB(80, 200, 180)   /* teal Style */
#define TACK_COL_SND   RGB(100, 180, 255)  /* ice Sounds */
#define TACK_COL_REC   RGB(255, 70, 70)    /* LOOP Rec row */
#define TACK_COL_QNT   RGB(80, 200, 140)   /* LOOP Quantize */
#define TACK_COL_CHD   RGB(120, 180, 255)  /* LOOP Apply */
#define TACK_COL_RHY   RGB(255, 180, 70)   /* LOOP Rhythm */
#define TACK_COL_LP    RGB(255, 140, 200)  /* LOOP Loop row */

static uint16_t tack_style_col(uint8_t si)
{
    /* Style chips — keep/strengthen; Beatles = Mersey-blue. */
    static const uint16_t COL[11] = {
        RGB(180, 190, 210), /* CLASSIC */
        RGB(200, 160, 80),  /* JAZZ gold */
        RGB(80, 220, 120),  /* JUNGLE green */
        RGB(80, 200, 100),  /* REGGAE green */
        RGB(140, 160, 200), /* AMBIENT cool */
        RGB(160, 100, 60),  /* DUB brown */
        RGB(255, 120, 60),  /* BREAKS orange */
        RGB(220, 180, 120), /* BOSSA sand */
        RGB(200, 140, 180), /* WALTZ rose */
        RGB(255, 140, 60),  /* FUNK orange */
        RGB(70, 130, 200),  /* BEATLES Mersey-blue */
    };
    if (si >= 11u) si = 0u;
    return COL[si];
}

static uint16_t tack_page_col(uint8_t pg)
{
    if (pg == TACK_PG_GEN) return TACK_COL_GEN;
    if (pg == TACK_PG_LOOP) return TACK_COL_LOOP;
    if (pg == TACK_PG_LEARN) return TACK_COL_LEARN;
    if (pg == TACK_PG_ZEN) return TACK_COL_ZEN;
    if (pg == TACK_PG_SOUNDS) return TACK_COL_SND;
    if (pg == TACK_PG_STYLE) return TACK_COL_STYLE;
    if (pg == TACK_PG_PRESET) return TACK_COL_SND;
    return T_THEME;
}


static uint16_t tack_loop_row_col(uint8_t row)
{
    switch (row & 3u) {
    case 0: return TACK_COL_REC;
    case 1: return TACK_COL_LP;
    case 2: return TACK_COL_QNT;
    default: return TACK_COL_RHY;
    }
}

static uint16_t tack_ab_col(uint8_t is_b)
{
    return is_b ? TACK_COL_B : TACK_COL_A;
}

static uint8_t tack_home_show_more(void);

/* 6.0: Dark erased. Legacy (5.9) indices: 0..5 same; old Dark(6)→Ambient; old≥7 → −1. */
static uint8_t tack_style_migrate(uint8_t st)
{
    if (st == 0xFFu) return 0xFFu;
    if (st == 6u) return (uint8_t)TACK_STYLE_AMBIENT; /* was DARK */
    if (st >= 7u && st <= 11u) return (uint8_t)(st - 1u);
    if (st >= TACK_STYLE_COUNT) return (uint8_t)TACK_STYLE_CLASSIC;
    return st;
}

static void tack_perf_save(uint8_t i);
static void tack_perf_recall(uint8_t i);
static void tack_queue_perf(uint8_t i);
static void tack_perf_seed_starters(void);
static void tack_learn_apply_cc(uint8_t cc, uint8_t val);
static void tack_midi_apply_levels(void); /* LEARN VOL/EXPR before def */
static void tack_zen_enter(void);
static void tack_zen_exit(void);
static void tack_zen_apply(void);
static void tack_apply_mute_solo(void);
static void tack_open_page(uint8_t pg);

static uint8_t tack_is_locked(void);
/* MIDI from controller — CC7 master, CC11 expression, FX CCs → TACK wet. */
static uint8_t tack_midi_master; /* 0..127 CC7 master scale for all lanes */
static uint8_t tack_midi_expr;   /* 0..127 CC11 expression (default 127) */
static uint8_t tack_level_base[4]; /* pre-CC lane levels for re-scale */

static uint8_t tack_amb_tex;    /* Ambient soft pick; Dark/Sci-Fi still 0..11 */
#define TACK_AMB_TEX_N 12u      /* Dark Amb + Sci-Fi catalog size (unchanged) */
#define TACK_AMB_SOFT_N 24u     /* 6.13 Ambient soft analog/zen — double; no DX7/bells */
static const char *const TACK_AMB_TEX_NAME[TACK_AMB_SOFT_N] = {
    "Soft Tri", "Soft Saw", "Warm Sqr", "Drift Pad", "Harm Bed",
    "Low Drone", "Zen Air", "Quiet Blm", "Dark Glass", "Pulse Mist",
    "Harm Pad", "Soft PWM", "Tape Wash", "Harm Fog", "Velvet Tri",
    "Sub Bloom", "Slow Choir", "Dust Pad", "Amber Drone", "Soft Fifths",
    "Quiet Saw", "Cloud Bed", "Deep Wash", "BRIGHT~"
};
/* Dark Ambient — analog HUGE Oberheim-like (size/weight, not Ambient+dark labels). */
static const char *const TACK_DARK_TEX_NAME[TACK_AMB_TEX_N] = {
    "Fat Stack", "OB Wide", "Huge Saw", "Stack Det", "Poly Mass",
    "Low Mass", "Slow Sweep", "Brass Wall", "Dark Choir", "Filter Bed",
    "Unison Bed", "MASS LIFT~"
};
/* Sci-Fi — F/X space: drones, spatial, auto-pan, weird space (not DX7/bell stubs). */
static const char *const TACK_SCIFI_TEX_NAME[TACK_AMB_TEX_N] = {
    "Space Drone", "Auto Pan", "Far Field", "Weird Orbit", "Void Bed",
    "Stereo Sweep", "Long Trail", "Radio Hiss", "Grav Well", "Spin Pad",
    "Deep Space", "STAR LIFT~"
};
static uint8_t tack_drm_dist;   /* drum-bus P_DIST amount mirrored for FX UI */
static uint8_t tack_drm_comp;   /* drum-bus softclip glue (P_E7 DRV) mirrored for FX UI */
/* FX page wet amounts — drive per-track sends (P_DLY/P_REV/P_CHOR). Bus knobs alone
 * were inaudible: no G_RMIX (reverb wet = send only), G_DMIX defaults near max. */
static uint8_t tack_fx_dly;     /* 0..127 delay wet (master → P_DLY + G_DMIX) */
static uint8_t tack_fx_rev;     /* 0..127 reverb wet (master → P_REV + G_RSIZE) */
static uint8_t tack_fx_cho;     /* 0..127 chorus wet (master → P_CHOR + G_CDEPTH) */
static uint8_t tack_fx_flange;  /* 0..127 voice FLANGER amount (IT_FLANGER on voices) */
static uint8_t tack_fx_crush;   /* 0..127 voice BIT REDUC amount (IT_CRUSH on voices; != drum crush) */
static uint8_t tack_fx_vcho;    /* 0..127 voice CHORUS amount (IT_CHORUS on voices; != send CHO) */
static uint8_t tack_fx_chaos;   /* 0..127 voice CHAOS — KAOSS combo fold/fuzz+phaser+filter (!= GEN CHAOS) */
static uint8_t tack_fx_tape;    /* 0..127 short tape delay (G_DTIME short + wet) */
/* Undo stack: last chord takes (4 slots each) */
static TackChord tack_undo[4][TACK_SLOTS];
static uint8_t tack_undo_n;
static uint32_t tack_seed_base;

static const char *const TACK_STYLE_NAME[TACK_STYLE_COUNT] = {
    "CLASSIC","JAZZ","JUNGLE","REGGAE","AMBIENT","DUB","BREAKS","BOSSA","WALTZ","FUNK","BEATLES"
};
/* Master tempos per style — full-track G_BPM (drums + engines + synced FX), ≤140. */
static const uint8_t TACK_STYLE_BPM[TACK_STYLE_COUNT] = {
    /* CLASSIC JAZZ JUNGLE REGGAE AMBIENT DUB BREAKS BOSSA WALTZ FUNK BEATLES */
    /* Jazz/Jungle/Reggae frozen. Tech/Trap/Punk/Dubstep/Dark removed.
     * Breaks 134; Bossa 112; Waltz 108 (3/4); Funk 100; Beatles 112 Mersey.
     * 6.0 index shift: styles after DUB (old≥7) are −1; old Dark(6)→Ambient on migrate. */
    88, 104, 140, 76, 66, 72, 134, 112, 108, 100, 112
};
/* TACK web nowTag / tagForChord — discreet harmonic role vs key root. */
static const char *const TACK_CHORD_TAG[12] = {
    "HOME", "SHADOW", "STEP", "FLOAT", "LIFT", "RELEASE",
    "SHIFT", "TENSION", "LIFT", "TURN", "DRIVE", "LEAN"
};
static const char *tack_chord_tag(const TackChord *c)
{
    uint8_t off = (uint8_t)((int)(c->root % 12u) - (int)(tack_state.root % 12u) + 12) % 12u;
    return TACK_CHORD_TAG[off];
}

/* TACK web functionFor — classical function vs key root (discreet under role tag). */
static const char *tack_chord_function(const TackChord *c)
{
    uint8_t off = (uint8_t)((int)(c->root % 12u) - (int)(tack_state.root % 12u) + 12) % 12u;
    switch (off) {
    case 0:  return "TONIC";
    case 1:  return "NEAP";
    case 3:  return "MED";
    case 5:  return "SUBDOM";
    case 7:  return "DOM";
    case 8:  return "SUBMED";
    case 10: return "SUBTON";
    default: return "COLOUR";
    }
}


static const char *const TACK_MODE_NAME[TACK_MODE_COUNT] = {
    "MAJOR","MINOR","DORIAN","PHRYGIAN","LYDIAN","CHINESE","CANTOPOP","JAPANESE",
    "WHOLE TONE","OCTATONIC","LOCRIAN","ULTRA","DISSONANT","HARMONIC MINOR",
    "PHRYGIAN DOM","BLUES","HEX BLUES","GLASS","REICH","RILEY","ADAMS"
};

static const char *const TACK_NOTES[12] = {
    "C","Db","D","Eb","E","F","Gb","G","Ab","A","Bb","B"
};

static const char *tack_display_suffix(uint8_t q)
{
    /* AF_S / AF_M are ASCII 32..126. The core's ° and °7 would be missing
     * glyphs, so the firmware spells those qualities. */
    if (q == TACK_Q_DIM) return "dim";
    if (q == TACK_Q_DIM7) return "dim7";
    if (q == TACK_Q_AUG) return "aug";
    return tack_chord_suffix(q);
}

static void tack_chord_text(char *out, uint32_t n, const TackChord *c)
{
    str_cpy(out, TACK_NOTES[c->root % 12u], n);
    str_cpy(out + str_len(out), tack_display_suffix(c->quality), n - str_len(out));
}

static void tack_clear(track_t *t)
{
    uint32_t i;
    for (i = 0; i < NSTEP; i++) {
        t->step[i].n = 0;
        t->step[i].time = ST_REST;
        t->step[i].hit = 0;
        t->step[i].acc = 0;
        t->step[i].flags = 0;
        t->step[i].vel = 0;
        t->step[i].probability = 0;
    }
}

static void tack_note(track_t *t, uint32_t i, const uint8_t *notes, uint32_t n, uint8_t vel)
{
    uint32_t k;
    if (i >= NSTEP) return;
    /* Cap at step_t.note[4] — FM-1 step poly ceiling. Caller passes full chord (≤4). */
    if (n > 4u) n = 4u;
    for (k = 0; k < n; k++) t->step[i].note[k] = notes[k];
    t->step[i].n = (uint8_t)n;
    t->step[i].time = ST_NOTE;
    t->step[i].vel = vel;
}

static void tack_tie(track_t *t, uint32_t from, uint32_t to)
{
    uint32_t i;
    for (i = from; i < to && i < NSTEP; i++) {
        t->step[i].n = 0;
        t->step[i].time = ST_TIE;
        t->step[i].hit = 0;
    }
}

/* Map browser MOODS onto Felucca engines/BPM/FX.
 * dens gates hat/perc/fill; global FX + P_CHOR follow air.fog / chorus / room.
 * AMBIENT keeps drums audible as a quiet kick (no mute); ENV FILL thickens. */
static void tack_apply_shimmer(void);
static void tack_apply_fx_scene(uint8_t sc);
static void tack_apply_drum_bus(void);
static void tack_apply_amb_texture(void);
static uint8_t tack_lib_sound_n(uint8_t bank);
static const char *tack_lib_sound_name(uint8_t bank, uint8_t idx);
static void tack_apply_voice_inserts(void);
static void tack_apply_fx_amounts(void);
static void tack_phrase_store(uint8_t ab);
static void tack_phrase_load(uint8_t ab);
static void tack_ab_toggle(void);
static void tack_arr_advance(void);
static uint8_t tack_mode_flavor(uint8_t mode);
typedef struct {
    int8_t dens_d;
    uint16_t kick_or;
    uint16_t kick_clr;
    uint16_t hat_or;
    uint16_t hat_clr;
    uint16_t snar_or;
    int8_t swing_d;
    uint8_t bass_spare;
    uint8_t bass_offbeat;
} TackComboMod;
static TackComboMod tack_combo_mod(uint8_t sty, uint8_t flav);


static void tack_patterns(void);
static void tack_restart(void);
static void tack_flash(const char *msg);

/* Shift MIDI note by tack_octave (−2..+2). Used for harmonic lanes only. */
static uint8_t tack_oct_note(int n)
{
    n += (int)tack_octave * 12;
    if (n < 0) n = 0;
    if (n > 127) n = 127;
    return (uint8_t)n;
}

static void tack_set_octave(int8_t o)
{
    char msg[12];
    if (o < -2) o = -2;
    if (o > 2) o = 2;
    if (o == tack_octave) return;
    tack_octave = o;
    song.octave = o; /* keep Felucca global in sync for LEDs / persist */
    tack_patterns();
    tack_restart();
    /* 6.12: soft redraw via content sig — no full wipe */
    if (o == 0) tack_flash("OCT 0");
    else if (o > 0) {
        msg[0] = 'O'; msg[1] = 'C'; msg[2] = 'T'; msg[3] = ' '; msg[4] = '+';
        msg[5] = (char)('0' + o); msg[6] = 0;
        tack_flash(msg);
    } else {
        msg[0] = 'O'; msg[1] = 'C'; msg[2] = 'T'; msg[3] = ' '; msg[4] = '-';
        msg[5] = (char)('0' + (-o)); msg[6] = 0;
        tack_flash(msg);
    }
}


/* Full polyphony for chord / looper path.
 * Comp (trk0) and air (trk2) must stay V_POLY so hand-played and looped chords
 * voice every tone written into the step (up to step_t.note[4] = 4).
 * Bass may stay MONO/LEGATO (single notes). Never leave a mono preset's V_LEGATO
 * on the chord bed — that collapses 4-note steps to one voice (paraphonic feel).
 *
 * Hardware ceilings (document in GATE/CHANGELOG):
 *   NVOICE=8 shared sounding budget across all parts (voice.c)
 *   FM6 engine_t.poly=6 per part
 *   step_t.note[4] — max 4 chord tones per step
 * Soft steal prefers released voices; when the budget is full, oldest non-bass
 * POLY holds may still yield — use the max cleanly, do not force mono. */
static void tack_assert_poly_chords(void)
{
    trk[0].p[P_VOICE] = V_POLY;
    trk[2].p[P_VOICE] = V_POLY;
    /* REUSE keeps chord tones on stable voices across re-triggers (less thrash). */
    trk[0].p[P_ALLOC] = 1; /* REUSE */
    trk[2].p[P_ALLOC] = 1;
}

/* Classic kits early — apply_style / bank / amb re-assert. */
static const uint8_t TACK_KIT_PRESET[4] = {4, 5, 6, 7}; /* 808 909 CR-78 707 */
static const char *const TACK_KIT_TAG[4] = {"808", "909", "CR78", "707"};

static void tack_apply_style(void)
{
    /* trk: 0=comp, 1=bass, 2=air/hook, 3=drums. eng 0=ANALOG, 12=FM6. */
    /* Genre-accurate tempos from TACK_STYLE_BPM (master G_BPM for full arrangement). */
    static const uint8_t SWING[TACK_STYLE_COUNT] = {0,55,0,6,0,4,0, 18, 8, 12, 8 /* Beatles */};
    /* Breaks≈Jungle-simple; Ambient zen-spare; Dark sparse; Jazz/Jungle/Reggae frozen.
     * Tech/Trap/Punk/Dubstep removed. */
    static const uint8_t DENS[TACK_STYLE_COUNT]  = {36,44,34,38,10,30,30, 28, 26, 24, 22 /* Beatles */}; /* Breaks≈Jungle-simple dens */
    /* COMP: analog vs FM; AIR pads lean soft / dark / bell for ambient worlds. */
    /* Waltz COMP = ANALOG Soft Saw (preset SOFT PAD) — soft wave, not FM6 BELL. */
    static const uint8_t COMP_E[TACK_STYLE_COUNT] = {0,ENGI_FM6,ENGI_FM6,ENGI_FM6,0,ENGI_FM6,0, ENGI_FM6, 0, 0, 0 /* Beatles */};
    /* Dub: PAD(4) not BELL(1) on FM6 — no bells. Ambient soft WIND bed. Jazz/Jungle/Reggae frozen. */
    static const uint8_t COMP_P[TACK_STYLE_COUNT] = {1,0,7,5,10,4,1, 0, 1, 1, 1 /* Beatles */}; /* Amb WIND; Dub PAD; Waltz Soft Saw(1); no BELL */
    static const uint8_t BASS_E[TACK_STYLE_COUNT] = {0,ENGI_FM6,ENGI_FM6,ENGI_FM6,0,0,0, ENGI_FM6, ENGI_FM6, 0, 0 /* Beatles */};
    /* Trap/Tech bass = ANALOG ACID; Punk SUB; others FM/sub. */
    static const uint8_t BASS_P[TACK_STYLE_COUNT] = {7,2,2,2,7,7,7, 2, 2, 7, 7 /* Beatles */}; /* Breaks SUB */
    /* Ambient/Dark air = ANALOG WIND/STRINGS; others FM6 PAD */
    static const uint8_t AIR_E[TACK_STYLE_COUNT]  = {ENGI_FM6,ENGI_FM6,ENGI_FM6,ENGI_FM6,0,0,ENGI_FM6, ENGI_FM6, ENGI_FM6, ENGI_FM6, ENGI_FM6 /* Beatles */};
    /* Ambient→WIND(10), Dub→STRINGS(11); else PAD(4). Waltz air stays FM6 PAD (wave bed). */
    static const uint8_t AIR_P[TACK_STYLE_COUNT]  = {4,4,4,4,10,11,4, 4, 4, 4, 4 /* Beatles */};
    /* Drum: 0 STD,1 DEEP,2 TIGHT,3 PERC,4 808,5 909 — Punk TIGHT */
    static const uint8_t DRM_P[TACK_STYLE_COUNT]  = {1,2,2,3,1,1,2, 6, 2, 4, 1 /* Beatles DEEP */}; /* Breaks TIGHT like Jungle */
    /* Per-track delay/reverb sends — tuned to MOODS air.fog / space */
    /* Track sends — raised so delay/reverb are clearly hearable vs dry (3.7). */
    static const uint8_t DLY[TACK_STYLE_COUNT][4] = {
        {55,18,78,12},
        {48,16,68,10},
        {40,14,56,10},
        {58,20,82,16},
        {96,42,124,14},
        {78,30,120,40},
        {36,12,52,10},
        {52,18,70,12},
        {48,16,74,14},
        {40,14,58,10},
        {46,16,64,12} /* Beatles warm room */
    };
    static const uint8_t REV[TACK_STYLE_COUNT][4] = {
        {82,28,98,40},
        {70,24,86,36},
        {58,22,72,32},
        {74,32,92,48},
        {124,72,126,64},
        {112,52,124,78},
        {48,16,58,24},
        {78,30,92,36},
        {86,32,100,40},
        {56,20,68,28},
        {72,26,84,34} /* Beatles soft Abbey */
    };
    /* air.chorus — Ambient/Dark slow/weighty; Punk present */
    /* Ambient zen: deep slow chorus; Tech/House present; Jazz/Jungle/Reggae frozen. */
    static const uint8_t AIR_CHO[TACK_STYLE_COUNT] = {36,42,26,48,42,50,28, 44, 36, 28, 30 /* Beatles */}; /* Amb quieter air; Breaks lean */
    /* Ambient near-zero zen; Jazz/Jungle/Reggae frozen. Tech/Trap/Punk/Dubstep removed. */
    static const uint8_t DIST[TACK_STYLE_COUNT] = {14,8,22,10,4,16,6, 6, 8, 10, 6 /* Beatles */};
    /* mood.comp.level / tone-ish ADSR */
    static const uint8_t LVL[TACK_STYLE_COUNT][4] = {
        {90,100,70,104},
        {88,98,68,104},
        {88,100,64,104},
        {90,102,72,100},
        {78,92,58,48},
        {96,104,70,92},
        {94,104,58,108},
        {88,98,66,96},
        {90,96,68,92},
        {92,104,60,100},
        {90,100,64,96} /* Beatles open mix */
    };
    /* Ambient zen: slower attack / long release. Punk tight short. Jazz/Jungle/Reggae frozen. */
    static const uint8_t ATK[TACK_STYLE_COUNT] = {18,8,4,6,80,28,4, 10, 12, 6, 12 /* Beatles */};
    static const uint8_t REL[TACK_STYLE_COUNT] = {90,70,36,36,124,105,40, 80, 88, 50, 72 /* Beatles */};
    /* Global FX bus — G_DTIME index into N_DTIME, G_RTYPE 0 ROOM / 1 SPRING / 2 HALL */
    static const uint8_t G_DT[TACK_STYLE_COUNT]   = {1,1,1,6,6,6,1, 1, 6, 1, 1 /* Beatles */};
    static const uint8_t G_DF[TACK_STYLE_COUNT]   = {52,48,50,64,84,88,44, 56, 60, 48, 54 /* Beatles */};
    static const uint8_t G_DM[TACK_STYLE_COUNT]   = {98,92,88,104,124,122,86, 96, 100, 90, 94 /* Beatles */};
    static const uint8_t G_RT[TACK_STYLE_COUNT]   = {2,2,0,1,2,1,0, 2, 2, 0, 2 /* Beatles HALL */};
    static const uint8_t G_RS[TACK_STYLE_COUNT]   = {88,80,68,92,126,116,62, 84, 90, 70, 82 /* Beatles */};
    static const uint8_t G_RD[TACK_STYLE_COUNT]   = {42,46,52,36,12,20,56, 38, 34, 48, 40 /* Beatles */};
    static const uint8_t G_CD[TACK_STYLE_COUNT]   = {48,55,32,58,48,60,28, 50, 42, 36, 38 /* Beatles */}; /* Amb chorus depth subtle */
    static const uint8_t G_CR[TACK_STYLE_COUNT]   = {28,34,28,32,12,18,28, 30, 22, 26, 24 /* Beatles */};
    uint8_t s = tack_style_idx < TACK_STYLE_COUNT ? tack_style_idx : 0;
    uint32_t i;

    song.g[G_CLOCK] = 0;                         /* internal master clock — style BPM owns the full track */
    song.g[G_BPM] = (int16_t)TACK_STYLE_BPM[s];
    if (song.g[G_BPM] > 140) song.g[G_BPM] = 140; /* hard ceiling */
    song.g[G_SWING] = (int16_t)SWING[s];
    {
        TackComboMod cm = tack_combo_mod(s, tack_mode_flavor(tack_mode_idx));
        int16_t sw = (int16_t)song.g[G_SWING] + cm.swing_d;
        if (sw < 0) sw = 0;
        if (sw > 100) sw = 100;
        song.g[G_SWING] = sw;
    }
    tack_dens = DENS[s];

    song.g[G_DTIME]  = (int16_t)G_DT[s];
    song.g[G_DFDBK]  = (int16_t)G_DF[s];
    song.g[G_DMIX]   = (int16_t)G_DM[s];
    song.g[G_RTYPE]  = (int16_t)G_RT[s];
    song.g[G_RSIZE]  = (int16_t)G_RS[s];
    song.g[G_RDAMP]  = (int16_t)G_RD[s];
    song.g[G_CDEPTH] = (int16_t)G_CD[s];
    song.g[G_CRATE]  = (int16_t)G_CR[s];

    set_engine_of(&trk[0], COMP_E[s]); apply_preset_to(&trk[0], COMP_P[s]);
    set_engine_of(&trk[1], BASS_E[s]); apply_preset_to(&trk[1], BASS_P[s]);
    trk[1].p[P_VOICE] = V_LEGATO; /* 6.13 spare bass mono/legato */
    if (trk[1].p[P_GLIDE] < 18) trk[1].p[P_GLIDE] = 18;
    if (!tack_air_user) {
        set_engine_of(&trk[2], AIR_E[s]);  apply_preset_to(&trk[2], AIR_P[s]);
    }
    set_engine_of(&trk[3], ENGI_DRUM); apply_preset_to(&trk[3], DRM_P[s]);
    /* 6.0 Waltz Soft Saw: slow bloom, LPF, gentle detune — never bell. */
    if (s == TACK_STYLE_WALTZ) {
        if (trk[0].eng_req == 0) {
            trk[0].p[P_E0] = 0;   /* SAW */
            trk[0].p[P_E1] = 18;  /* soft DTN */
            trk[0].p[P_E4] = 52;  /* LPF */
            trk[0].p[P_E5] = 18;  /* low RES */
            trk[0].p[P_ATK] = 72; trk[0].p[P_REL] = 110;
            trk[0].p[P_CHOR] = 36; trk[0].p[P_REV] = 88;
        }
        if (trk[2].eng_req == ENGI_FM6 && trk[2].preset == 4) {
            trk[2].p[P_ATK] = 80; trk[2].p[P_REL] = 118;
            trk[2].p[P_CHOR] = 40; trk[2].p[P_REV] = 96;
        }
    }


    for (i = 0; i < 4u; i++) {
        trk[i].p[P_DLY] = DLY[s][i];
        trk[i].p[P_REV] = REV[s][i];
        trk[i].p[P_LEVEL] = LVL[s][i];
        tack_level[i] = trk[i].p[P_LEVEL];
        tack_level_base[i] = (uint8_t)tack_level[i];
        trk[i].p[P_PAN] = (int16_t)tack_lane_pan[i]; /* lane mixer pan survives style */
    }
    /* Comp envelope from mood.compMode (ballad vs stabs/pulse) */
    trk[0].p[P_ATK] = ATK[s];
    trk[0].p[P_REL] = REL[s];
    trk[1].p[P_ATK] = (int16_t)(ATK[s] / 2u + 2u);
    trk[1].p[P_REL] = (int16_t)(REL[s] > 20u ? REL[s] - 20u : REL[s]);
    /* Air: longer pad + chorus from air.chorus; fog already in REV send */
    trk[2].p[P_ATK] = (int16_t)(20u + ATK[s]);
    trk[2].p[P_REL] = (int16_t)(80u + REL[s] / 2u);
    trk[2].p[P_CHOR] = AIR_CHO[s];
    trk[2].p[P_SUS] = 100;
    /* Band grit on the whole chain (comp/bass/air). Tech DIST[]=0 → clean. */
    trk[0].p[P_DIST] = DIST[s];
    trk[1].p[P_DIST] = (uint8_t)((DIST[s] * 3u) / 4u); /* bass almost as dirty */
    trk[2].p[P_DIST] = (uint8_t)((DIST[s] * 2u) / 3u); /* air grit */
    /* Seed FX-page wet masters from style, then push to P_DLY/P_REV/P_CHOR + bus. */
    tack_fx_dly = DLY[s][2];
    tack_fx_rev = REV[s][2];
    tack_fx_cho = AIR_CHO[s];
    if (song.g[G_DCOLOR] < 40) song.g[G_DCOLOR] = 70; /* delay feedback colour — never dead-dark */
    tack_apply_fx_amounts();
    /* Named FX scene lock survives style — independent of style. */
    if (tack_fx_scene_lock) tack_apply_fx_scene(tack_fx_scene);

    /* Drum-bus distortion + softclip compressor (P_DIST character, P_E7 DRV glue).
     * Controllable — not trashy. Ambient/Dark gentle; Trap/Tech/Jungle/Dark more grit. */
    {
        /* Tech: almost no dist, strong COMP. Trap/Punk grit. Ambient zen soft. Jazz/Jungle/Reggae frozen. */
        /* 5.1: crush+comp clearly audible (Drums page CRUSH/COMP); Ambient still soft. */
        static const uint8_t DRM_DIST[TACK_STYLE_COUNT] = {28,18,36,20,8,22,18, 16, 12, 22, 14 /* Beatles soft */};
        static const uint8_t DRM_COMP[TACK_STYLE_COUNT] = {48,40,56,42,18,44,64, 38, 34, 50, 36 /* Beatles */};
        tack_drm_dist = DRM_DIST[s];
        tack_drm_comp = DRM_COMP[s];
        tack_apply_drum_bus();
    }
    /* 6.1+ decoupling: style never forces library (BREAKS no longer writes snd_bank). */
    /* Amb/Dark: tame WIND/noise “waves” so pads/kick/FX stay audible. */
    if (s == TACK_STYLE_AMBIENT) {
        /* Analog WIND edit: WAVE DTN MIX NOIS CUT RES DRV KTR → lower NOIS/RES/DRV */
        if (trk[0].eng_req == 0 && trk[0].preset == 10) {
            trk[0].p[P_E3] = 28;  /* NOIS soft */
            trk[0].p[P_E5] = 36;  /* RES calm */
            trk[0].p[P_E6] = 0;
            trk[0].p[P_DIST] = 8;
            if (trk[0].p[P_LEVEL] > 70) trk[0].p[P_LEVEL] = 62;
            tack_level[0] = trk[0].p[P_LEVEL];
        }
        if (trk[2].eng_req == 0 && (trk[2].preset == 10 || trk[2].preset == 11)) {
            trk[2].p[P_E3] = 22;  /* NOIS under mix */
            trk[2].p[P_E5] = 30;
            trk[2].p[P_E6] = 0;
            trk[2].p[P_DIST] = 4;
            trk[2].p[P_CHOR] = 36;
            if (trk[2].p[P_LEVEL] > 64) trk[2].p[P_LEVEL] = 52;
            tack_level[2] = trk[2].p[P_LEVEL];
        }
        /* Kill loud mod slots that sweep the whole bed */
        trk[0].p[P_M1AMT] = 18; trk[0].p[P_M2AMT] = 12;
        trk[2].p[P_M1AMT] = 16; trk[2].p[P_M2AMT] = 10;
        trk[0].p[P_LRATE] = 18; trk[2].p[P_LRATE] = 14; /* slow quiet LFO */
        song.g[G_CDEPTH] = 40;
        song.g[G_CRATE] = 10;
    }

    /* AMBIENT: drums stay on at a discreet level (quiet kick only in patterns).
     * Style entry resets FILL so the default is always kick-only; ENV thickens. */
    tack_mute &= (uint8_t)~(1u << 3);
    if (s == TACK_STYLE_AMBIENT) {
        tack_fill = 0;
        trk[3].p[P_LEVEL] = 40; /* Amb zen quieter */
        tack_level[3] = trk[3].p[P_LEVEL];
        /* Force sombre mode on Ambient (defaults stay serious). */
        if (tack_mode_idx == TACK_MODE_MAJOR)
            { tack_mode_idx = TACK_MODE_MINOR; tack_set_mode_keep(&tack_state, tack_mode_idx, tack_slot_owned); }
    } else {
        /* Leaving ambient worlds: restore a usable drum level if it was ducked. */
        if (tack_level[3] < 72) tack_level[3] = 100;
        trk[3].p[P_LEVEL] = tack_level[3];
        tack_mute &= (uint8_t)~(1u << 3);
    }
    /* Style-tied air — freeze locks; morph eases chorus over a few bars. */
    if (!tack_air_freeze) {
        uint8_t want_sh = 0;
        if (s == TACK_STYLE_AMBIENT) want_sh = 1; /* subtle — waves under mix */
        else if (s == TACK_STYLE_DUB) want_sh = 1;
        else if (s == TACK_STYLE_BREAKS) want_sh = 0;
        tack_shimmer = want_sh;
        if (tack_air_morph && song.playing) {
            tack_air_target = AIR_CHO[s];
            tack_morph_left = (uint8_t)(2u + tack_air_morph * 2u);
        } else {
            song.g[G_CDEPTH] = (int16_t)AIR_CHO[s];
            tack_morph_left = 0;
        }
        tack_apply_shimmer();
    }
    /* Style load re-seeds drums — release PART ownership (same independence as kit/lib). */
    tack_drm_part_own = 0;
    /* Kit independence: style DRM preset must not wipe a user-held classic kit. */
    if (tack_kit_sel <= 3u) {
        apply_preset_to(&trk[3], TACK_KIT_PRESET[tack_kit_sel]);
        tack_beat_kit = (tack_kit_sel >= 2u) ? (uint8_t)(tack_kit_sel - 1u) : 0u;
        tack_apply_drum_bus();
    }
    /* Chord beds stay POLY — never inherit mono preset V_LEGATO. */
    tack_assert_poly_chords();
    /* 6.12: soft dirty-region — chrome repaints without full wipe. */
}

/* Deterministic 0..255 from style/bar/step/loop/seed — no free randomness. */
static uint8_t tack_drum_hash(uint8_t bar, uint8_t step, uint8_t salt)
{
    uint32_t x = tack_state.seed ^ 0x5441434Bu;
    x ^= (uint32_t)tack_style_idx * 0x9E3779B1u;
    x ^= (uint32_t)bar * 127u + (uint32_t)step * 311u + (uint32_t)tack_loop * 74u + (uint32_t)salt * 19u;
    x *= 1664525u;
    x += 1013904223u;
    return (uint8_t)(x >> 24);
}

/* Browser BEAT_PATTERNS packed as 16-bit masks per lane: kick, snare/clap, hat, open, perc.
 * Order matches TACK_STYLE_* (CLASSIC .. BEATLES; Dark erased 6.0). */
/* Browser BEAT_PATTERNS packed as 16-bit masks per lane: kick, snare/clap, hat, open, perc.
 * Order matches TACK_STYLE_* (CLASSIC .. BEATLES; Dark erased 6.0).
 * TRAP: broken / syncopated (not straight 808 roll). TECH: hit on every beat (0,4,8,12).
 * AMBIENT: discreet kick only (step 0); hats/snares off unless ENV FILL. */
static const uint16_t TACK_KICK[TACK_STYLE_COUNT] = {0x0101,0x0441,0x0441,0x0100,0x0001,0x0100,0x0441, 0x0101, 0x0111, 0x0101, 0x0101};
static const uint16_t TACK_SNAR[TACK_STYLE_COUNT] = {0x1010,0x0490,0x1010,0x0100,0x0000,0x0100,0x1010, 0x1010, 0x0440, 0x1010, 0x1010};
static const uint16_t TACK_HAT[TACK_STYLE_COUNT] = {0x4444,0x4444,0x4444,0x4444,0x0000,0x0440,0x4444, 0x4444, 0x0000, 0x0404, 0x0404};
static const uint16_t TACK_OPEN[TACK_STYLE_COUNT] = {0x0000,0x4000,0x0000,0x0000,0x0000,0x4000,0x0000, 0x0000, 0x0000, 0x0000, 0x0000};
static const uint16_t TACK_PERC[TACK_STYLE_COUNT] = {0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000, 0x0000, 0x0000, 0x0000, 0x0000};


/* Mode flavor groups — systematic drum/bass nudges over the base style.
 * Minimal: small dens/mask/swing offsets, never a second busy layer. */
enum {
    TACK_FLAV_WEST = 0,   /* MAJOR MINOR DORIAN LYDIAN */
    TACK_FLAV_EAST,       /* CHINESE CANTOPOP JAPANESE */
    TACK_FLAV_DARK,       /* PHRYGIAN LOCRIAN ULTRA DISSONANT OCTATONIC WHOLETONE PHRYG DOM */
    TACK_FLAV_BLUES,      /* BLUES + HEX BLUES — spare drums, shuffle */
    TACK_FLAV_GLASS,      /* Philip Glass — additive ostinato, air-forward */
    TACK_FLAV_REICH,      /* Steve Reich — phase pulse, marimba-clear grid */
    TACK_FLAV_RILEY,      /* Terry Riley — In C cells, gradual dens */
    TACK_FLAV_ADAMS,      /* John Adams — post-minimal pulse drive */
    TACK_FLAV_COLOR       /* HARMONIC MINOR + remaining color */
};

static uint8_t tack_mode_flavor(uint8_t mode)
{
    switch (mode) {
    case TACK_MODE_CHINESE:
    case TACK_MODE_CANTOPOP:
    case TACK_MODE_JAPANESE:
        return TACK_FLAV_EAST;
    case TACK_MODE_PHRYGIAN:
    case TACK_MODE_LOCRIAN:
    case TACK_MODE_ULTRA:
    case TACK_MODE_DISSONANT:
    case TACK_MODE_OCTATONIC:
    case TACK_MODE_WHOLETONE:
    case TACK_MODE_PHRYGIAN_DOMINANT:
        return TACK_FLAV_DARK;
    case TACK_MODE_BLUES:
    case TACK_MODE_HEXATONIC_BLUES:
        return TACK_FLAV_BLUES;
    case TACK_MODE_GLASS:
        return TACK_FLAV_GLASS;
    case TACK_MODE_REICH:
        return TACK_FLAV_REICH;
    case TACK_MODE_RILEY:
        return TACK_FLAV_RILEY;
    case TACK_MODE_ADAMS:
        return TACK_FLAV_ADAMS;
    case TACK_MODE_HARMONIC_MINOR:
        return TACK_FLAV_COLOR;
    default:
        return TACK_FLAV_WEST;
    }
}

/* dens delta -12..+12, kick_or, kick_and(~clear), hat_or, swing_add, bass_spare(1=fewer hits) */


static TackComboMod tack_combo_mod(uint8_t sty, uint8_t flav)
{
    /* Default: no change. Rows are style-major, columns flavor — subtle only. */
    TackComboMod m = {0, 0, 0, 0, 0, 0, 0, 0, 0};
    if (flav == TACK_FLAV_WEST) return m;

    if (flav == TACK_FLAV_EAST) {
        /* East Asian over beat: lighter kick, more open space, soft offbeat hats. */
        m.dens_d = -8;
        m.kick_clr = 0x1010;      /* clear some mid kicks if present */
        m.hat_or = 0x0400;        /* soft mark at 10 */
        m.hat_clr = 0xAAAA;       /* thin odd 16ths */
        m.swing_d = 4;
        m.bass_spare = 1;
        if (sty == TACK_STYLE_JUNGLE) {
            m.dens_d = -12;
            m.snar_or = 0;        /* less snare chatter */
            m.kick_clr |= 0x0A04; /* fewer amen ghosts */
            m.bass_offbeat = 1;
        } else if (sty == TACK_STYLE_AMBIENT) {
            m.dens_d = -2;
        }
        return m;
    }
    if (flav == TACK_FLAV_DARK) {
        m.dens_d = -6;
        m.kick_clr = 0x0440;
        m.hat_clr = 0xAAAA;
        m.snar_or = 0x0000;
        m.swing_d = 0;
        m.bass_spare = 1;
        if (sty == TACK_STYLE_JUNGLE) { m.dens_d = -10; m.kick_clr |= 0x9292; }
        return m;
    }
    if (flav == TACK_FLAV_BLUES) {
        /* Spare drums, shuffle swing — blues sit, not busy. Style still owns G_BPM (≤140). */
        m.dens_d = -10;
        m.kick_clr = 0x1010;
        m.hat_clr = 0xAAAA;
        m.snar_or = 0x0000;
        m.swing_d = 10;
        m.bass_spare = 1;
        m.bass_offbeat = 1;
        if (sty == TACK_STYLE_JUNGLE) { m.dens_d = -12; m.kick_clr |= 0x9292; }
        if (sty == TACK_STYLE_AMBIENT) { m.dens_d = -4; m.swing_d = 4; }
        return m;
    }
    if (flav == TACK_FLAV_GLASS) {
        /* Additive/subtractive ostinato: very spare drums, no swing, air-forward. */
        m.dens_d = -14;
        m.kick_clr = 0x1450;
        m.hat_clr = 0xEEEE;
        m.swing_d = 0;
        m.bass_spare = 1;
        m.bass_offbeat = 1; /* arpeggio sit off the kick */
        if (sty == TACK_STYLE_AMBIENT) m.dens_d = -8;
        return m;
    }
    if (flav == TACK_FLAV_REICH) {
        /* Phase/pulse: steady grid, marimba-clear hats, minimal kick, canonic offset later. */
        m.dens_d = -16;
        m.kick_clr = 0x5554; /* leave only downbeats */
        m.hat_clr = 0x0000;  /* keep even 8ths — rhythmic clarity */
        m.snar_or = 0x0000;
        m.swing_d = 0;
        m.bass_spare = 1;
        if (sty == TACK_STYLE_BREAKS) { m.dens_d = -8; m.kick_clr |= 0x9292; } /* Jungle-simple syncopation */
        return m;
    }
    if (flav == TACK_FLAV_RILEY) {
        /* In C cells: spare start; dens creeps up with loop (gradual entry). */
        m.dens_d = (int8_t)(-14 + (int)(tack_loop % 5u) * 2); /* -14..-6 */
        m.kick_clr = 0x1450;
        m.hat_clr = 0xAAAA;
        m.swing_d = 0;
        m.bass_spare = 1;
        return m;
    }
    if (flav == TACK_FLAV_ADAMS) {
        /* Post-minimal: slightly fuller pulse, still sombre, soft drive. */
        m.dens_d = -8;
        m.kick_clr = 0x1010;
        m.hat_clr = 0x8888;
        m.swing_d = 0;
        m.bass_spare = 0;
        return m;
    }
    /* COLOR / harmonic minor: slight tension — rare extra perc bit via hat_or */
    m.dens_d = 2;
    m.hat_or = 0x2000;
    m.swing_d = 2;
    if (sty == TACK_STYLE_JAZZ || sty == TACK_STYLE_CLASSIC) m.bass_offbeat = 1;
    return m;
}

static uint8_t tack_bar_steps(void);
static uint8_t tack_loop_steps(void);
static uint8_t tack_seq_bar(void);
static uint8_t tack_seq_step16(void);
static void tack_drm_phrase_fills(void);
static void tack_rand_drm_fire(void); /* 6.19 alias → tack_drm_gen */
static void tack_drm_gen(void);
static void tack_drm_remix(void);
static void tack_drm_density_nudge(int dir);
static void tack_drm_lock_toggle(void);

static void tack_patterns(void)
{
    /* Browser COMP_RHYTHMS / BASS_RHYTHMS packed as 16-bit step masks.
     * Indices follow mood.comp.compMode / bassMode per style. */
    static const uint16_t COMP_MASK[TACK_STYLE_COUNT] = {0x0249,0x8244,0x1041,0x4444,0x1041,0x8244,0x1041, 0x8244, 0x0921, 0x1041, 0x0249};
    static const uint16_t BASS_MASK[TACK_STYLE_COUNT] = {0x0101,0x4441,0x0101,0x4441,0x0101,0x0101,0x0101, 0x0101, 0x0111, 0x0440, 0x0501};
    /* bassMode move roles at steps 0,6,10,14: R=0, 5th=7, 3rd=3/4, approach=-1 */
    static const int8_t MOVE_OFF[16] = {
        0,0,0,0,0,0,7,0,0,0,3,0,0,0,-1,0
    };
    uint32_t bar, s, i;
    uint8_t notes[4];
    const uint8_t sty0 = tack_style_idx < TACK_STYLE_COUNT ? tack_style_idx : 0;
    const uint8_t flav = tack_mode_flavor(tack_mode_idx);
    const TackComboMod cmod = tack_combo_mod(sty0, flav);
    uint16_t cmask = COMP_MASK[sty0];
    uint16_t bmask = BASS_MASK[sty0];
    /* 6.13: style-aware spare bass voice — always root/5th/approach on hits; never busy. */
    uint8_t bass_move = 1u;
    uint8_t dens_use;
    /* Spare flavors: keep note moves, thin the mask (roots 1+3). */
    if (cmod.bass_spare) {
        bmask = 0x0101; /* roots on 1 + 3 only */
    }
    if (cmod.bass_offbeat) {
        bmask = 0x0440; /* 6 + 10 — leave space under melody */
    }
    /* Beatles: spare root–fifth outline; every-bar tiny mask breathe (not a new system). */
    if (sty0 == TACK_STYLE_BEATLES) {
        bmask = (tack_loop & 1u) ? 0x0501u : 0x0101u; /* 1+3 roots; odd loops add mid 5th slot */
    }
    /* Frozen Jazz/Jungle/Reggae keep their style masks; moves stay on. */
    /* Every-bar small change: odd bars drop the approach slot (bit 14) — never denser. */
    if (tack_loop & 1u)
        bmask = (uint16_t)(bmask & (uint16_t)~0x4000u);
    /* Reich: canonic/phase offset — bass +2 16ths, comp +1 vs the pulse grid. */
    if (tack_mode_idx == TACK_MODE_REICH) {
        bmask = (uint16_t)(((bmask << 2) | (bmask >> 14)) & 0xFFFFu);
        cmask = (uint16_t)(((cmask << 1) | (cmask >> 15)) & 0xFFFFu);
    }
    /* Glass: additive process — grow comp cells with loop (1→2→3 hits / bar). */
    if (tack_mode_idx == TACK_MODE_GLASS) {
        uint8_t cells = (uint8_t)(1u + (tack_loop % 4u));
        uint16_t add = 0x0001u;
        if (cells >= 2u) add |= 0x0010u;
        if (cells >= 3u) add |= 0x0100u;
        if (cells >= 4u) add |= 0x1000u;
        cmask = add;
    }
    /* Riley: In C — stagger cell entry by bar (gradual pattern layering). */
    if (tack_mode_idx == TACK_MODE_RILEY) {
        /* early bars: thinner comp; later bars denser cells */
        if ((tack_loop & 3u) == 0u) cmask &= 0x0101u;
        else if ((tack_loop & 3u) == 1u) cmask &= 0x1111u;
    }
    dens_use = (uint8_t)clamp((int32_t)tack_dens + (int32_t)cmod.dens_d, 0, 100);
    if (tack_drm_spare) dens_use = (uint8_t)((dens_use * 2u) / 3u);
    if (tack_drm_hats == 0u) dens_use = (uint8_t)((dens_use * 3u) / 4u);
    else if (tack_drm_hats == 2u && dens_use < 92u) dens_use = (uint8_t)(dens_use + 12u);

    {
        uint8_t bs0 = tack_bar_steps();
        uint8_t loop_n = (uint8_t)(4u * bs0); /* Waltz 48, else 64 */
        for (i = 0; i < NTRK; i++) {
            track_t *t = &trk[i];
            /* 6.10: owned PART — never clear/stomp drum hits on style/bar flush. */
            if (i == 3u && tack_drm_part_own) {
                t->p[P_SLEN] = loop_n;
                t->p[P_QUANT] = 0;
                t->p[P_CHRD] = 0;
                continue;
            }
            tack_clear(t);
            t->p[P_SLEN] = loop_n;
            t->p[P_QUANT] = 0;
            t->p[P_CHRD] = 0; /* TACK writes full voicings itself (tack_note n=4) */
        }
        /* Full poly: comp+air voice every chord tone (≤4 / step; FM-1 NVOICE=8 shared). */
        tack_assert_poly_chords();
    }
    for (bar = 0; bar < 4u; bar++) {
        const uint8_t bs = tack_bar_steps();
        const uint32_t chord_bar = tack_hold ? tack_hold_bar : bar;
        const TackChord *c = &tack_state.chords[chord_bar];
        const TackChord *cn = &tack_state.chords[(chord_bar + 1u) & 3u];
        const uint32_t base = bar * (uint32_t)bs;
        uint8_t vel_c = (sty0 == TACK_STYLE_AMBIENT) ? 70u : 92u;

        uint8_t nvoices = 4u;
        tack_chord_notes(c, notes);
        {
            uint32_t ni;
            for (ni = 0; ni < 4u; ni++) notes[ni] = tack_oct_note((int)notes[ni]);
        }
        if (bar < 4u && tack_bar_lit_n[bar]) {
            TackScale sc;
            TackClassifiedEv ev;
            uint8_t mapped[TACK_REC_MAX_VOICES];
            int got, k;
            tack_fill_scale(&sc);
            ev.kind = (TackEvKind)tack_bar_lit_kind[bar];
            ev.n = tack_bar_lit_n[bar];
            ev.onset_ms = 0;
            ev.off_ms = 1;
            for (k = 0; k < ev.n && k < TACK_REC_MAX_VOICES; k++)
                ev.pitches[k] = tack_bar_lit[bar][k];
            got = tack_rec_playback_pitches(&ev, &sc, mapped, TACK_REC_MAX_VOICES);
            if (got > 0) {
                if (got > 4) { tack_rec_failed = 1; got = 0; }
                nvoices = (uint8_t)got;
                for (k = 0; k < got; k++)
                    notes[k] = mapped[k]; /* literal absolute pitch: never global-octave shifted */
            }
        }
        for (s = 0; s < bs; s++) {
            uint8_t bit_s = (uint8_t)(s % 16u); /* mask tables stay 16-bit; Waltz uses low 12 */
            if (cmask & (uint16_t)(1u << bit_s)) {
                uint8_t v = vel_c;
                if (s == 0u) v = (uint8_t)(v + 8u);
                else if (s + 4u >= bs) v = (uint8_t)(v - 16u);
                tack_note(&trk[0], base + s, notes, nvoices, v);
            }
        }

        {
            uint16_t bm = bmask;
            /* Spare: drop mid-bar root on odd bars — every-bar breathe, never busier. */
            if (cmod.bass_spare && (bar & 1u))
                bm = (uint16_t)(bm & (uint16_t)~0x0100u);
            /* Odd bars: prefer 5th over 3rd on move slots (tiny style-aware change). */
            for (s = 0; s < bs; s++) {
            uint8_t bit_s = (uint8_t)(s % 16u);
            if (bm & (uint16_t)(1u << bit_s)) {
                int8_t off = bass_move ? MOVE_OFF[s] : 0;
                int n;
                if (off == 3 && (bar & 1u))
                    off = 7; /* 3rd → 5th on odd bars */
                if (off < 0) {
                    /* approach next root from below */
                    n = (int)(36u + cn->root) - 1;
                    if (n < 24) n += 12;
                } else {
                    n = (int)(36u + c->root + (uint8_t)off);
                }
                while (n > 48) n -= 12;
                while (n < 24) n += 12;
                notes[0] = tack_oct_note(n);
                tack_note(&trk[1], base + s, notes, 1, (uint8_t)(s == 0u ? 104u : 88u));
            }
            }
            /* Mono/legato bass — single voice, tied feel; chords stay POLY via assert. */
            trk[1].p[P_VOICE] = V_LEGATO;
            if (trk[1].p[P_GLIDE] < 18) trk[1].p[P_GLIDE] = 18;
        }

        /* Ambient/Dark/Dub: mid register (weighty), not bright high pads at 72. */
        {
            uint8_t air_oct = 60u;
            if (sty0 == TACK_STYLE_AMBIENT) air_oct = 48u;
            else if (sty0 == TACK_STYLE_DUB) air_oct = 55u;
            notes[0] = tack_oct_note((int)air_oct + (int)c->root);
        }
        tack_note(&trk[2], base + 0, notes, 1, (uint8_t)(sty0 == TACK_STYLE_AMBIENT || sty0 == TACK_STYLE_DUB ? 70u : 58u));
        tack_tie(&trk[2], base + 1, base + bs);
        if (tack_state.mode == TACK_MODE_CANTOPOP) {
            for (s = 0; s < bs; s++) {
                int hn = tack_cantopop_hook_note(&tack_state, (uint8_t)bar, (uint8_t)(s % 16u));
                if (hn >= 0) {
                    trk[2].step[base + s].note[1] = tack_oct_note(hn);
                    trk[2].step[base + s].n = 2;
                    trk[2].step[base + s].time = ST_NOTE;
                    trk[2].step[base + s].vel = 64;
                }
            }
        }

        /* 6.10: style/song-list drums seed only when PART not user-owned. */
        if (!tack_drm_part_own) {
        /* Style kit + dens + per-bar plan (browser drumPlan / BEAT_PATTERNS). */
        {
            const uint8_t sty = sty0;
            const uint8_t dens = dens_use;
            const uint8_t ambient = (sty == TACK_STYLE_AMBIENT);
            const uint8_t lift = (bar >= 2u);
            const uint8_t half = (bar == 1u || bar == 3u);
            const uint8_t last = (bar == 3u);
            uint16_t kick_m = (uint16_t)((TACK_KICK[sty] | cmod.kick_or) & (uint16_t)~cmod.kick_clr);
            uint16_t snar_m = (uint16_t)(TACK_SNAR[sty] | cmod.snar_or);
            uint16_t hat_m  = (uint16_t)((TACK_HAT[sty] | cmod.hat_or) & (uint16_t)~cmod.hat_clr);
            /* Kick lock: downbeats only — never rewrite Jazz/Jungle/Reggae masks. */
            if (tack_kick_lock
                && sty != TACK_STYLE_JAZZ && sty != TACK_STYLE_JUNGLE
                && sty != TACK_STYLE_REGGAE)
                kick_m &= 0x1111u;
            /* Quiet hats: thin hat mask; MORE leaves mask, dens already bumped. */
            if (tack_drm_hats == 0u) hat_m &= 0x4444u;
            if (tack_drm_spare) { hat_m &= 0x4444u; snar_m &= (uint16_t)~0x00A0u; }
            /* CR-78 / 707 classic spare beats — extra choices; never overwrite Jazz/Jungle/Reggae. */
            if (tack_beat_kit && sty != TACK_STYLE_JAZZ && sty != TACK_STYLE_JUNGLE
                && sty != TACK_STYLE_REGGAE) {
                if (tack_beat_kit == 1u) { /* CR-78: soft 1+3 kick, rim 2+4, light hats */
                    kick_m = 0x0101u; snar_m = 0x1010u; hat_m = 0x4444u; 
                } else if (tack_beat_kit == 2u) { /* 707: spare 1+3 / 2+4, airy hats */
                    kick_m = 0x0101u; snar_m = 0x1010u; hat_m = 0x0404u;
                }
            }
            /* Bossa: soft kick+rim, clave-ish — style masks already light; keep air. */
            if (sty == TACK_STYLE_BOSSA) {
                kick_m = 0x0101u; snar_m = 0x1010u; hat_m = 0x4444u;
            }
            /* Waltz 3/4: oom-pah-pah in 12 sixteenths — kick on 1, soft on 2+3. */
            if (sty == TACK_STYLE_WALTZ) {
                kick_m = 0x0111u; /* steps 0,4,8 within low 12 */
                snar_m = 0x0000u;
                hat_m = 0x0440u;  /* light pah on 2+3 */
            }
            /* Funk: spare pocket — kick 1+3, snare 2+4, thin hats (no spray). */
            if (sty == TACK_STYLE_FUNK) {
                kick_m = 0x0101u; snar_m = 0x1010u; hat_m = 0x0404u;
            }
            /* Beatles: Ringo-ish spare — kick 1+3, snare 2+4, soft closed hats; no bell/cowbell. */
            if (sty == TACK_STYLE_BEATLES) {
                kick_m = 0x0101u; snar_m = 0x1010u; hat_m = 0x0404u;
            }
            const uint16_t open_m = TACK_OPEN[sty];
            const uint16_t perc_m = TACK_PERC[sty];

            if (ambient && !tack_fill) {
                /* Discreet kick only: step 0 every bar; every other bar softens further via vel.
                 * No hats / snares / perc. */
                for (s = 0; s < bs; s++) {
                    step_t *st = &trk[3].step[base + s];
                    uint16_t bit = (uint16_t)(1u << (s % 16u));
                    if (kick_m & bit) {
                        /* skip odd bars for extra space (every other bar) */
                        if ((bar & 1u) == 0u || dens > 15u) {
                            st->hit = 1u;
                            st->time = ST_NOTE;
                            st->vel = (uint8_t)((bar & 1u) ? 36u : 48u);
                            st->acc = 0;
                        }
                    }
                }
            } else for (s = 0; s < bs; s++) {
                step_t *st = &trk[3].step[base + s];
                uint8_t hit = 0;
                uint8_t h1 = tack_drum_hash((uint8_t)bar, (uint8_t)s, 1);
                uint8_t h2 = tack_drum_hash((uint8_t)bar, (uint8_t)s, 2);
                uint8_t h3 = tack_drum_hash((uint8_t)bar, (uint8_t)s, 3);
                uint8_t h4 = tack_drum_hash((uint8_t)bar, (uint8_t)s, 4);
                uint8_t h5 = tack_drum_hash((uint8_t)bar, (uint8_t)s, 5);
                uint16_t bit = (uint16_t)(1u << (s % 16u));

                if (kick_m & bit) hit |= 1u;
                if ((snar_m & bit) && dens > 10u) hit |= 2u;
                if ((hat_m & bit) && (s % 2u == 0u || dens > 30u)) hit |= (1u << 3);
                if ((open_m & bit) && dens > 30u) hit |= (1u << 4);
                if ((perc_m & bit) && dens > 18u) hit |= (1u << 6);


                /* Hat/fill ratchets ×2/×3/×4 — generative denser 16ths (not stock note grid). */
                {
                    static uint8_t rat_left;
                    if (s == 0u) rat_left = 0;
                    if (rat_left) {
                        hit |= (1u << 3);
                        rat_left--;
                    } else if (tack_drm_ratchet && (hit & (1u << 3))) {
                        rat_left = tack_drm_ratchet; /* 1→×2, 2→×3, 3→×4 extras */
                    } else if (tack_drm_ratchet && tack_fill && last && s >= 12u && (s & 1u) == 0u) {
                        hit |= (1u << 3);
                        rat_left = tack_drm_ratchet;
                    }
                }

                /* Living drums (TACK web feel): phrase/loop-aware evolution — not a stuck loop.
                 * Hats move a lot; kicks small except Tech/Dark/House 4OTF locked; Jazz/Jungle/Reggae
                 * patterns frozen at mask+light touch; no cowbell. */
                if (!ambient) {
                    /* Phrase salt: changes every 4 bars via tack_loop so patterns breathe. */
                    uint8_t ph = (uint8_t)(tack_loop & 3u);
                    uint8_t live = (uint8_t)(18u + ph * 10u + (lift ? 14u : 0u) + (half ? 8u : 0u));
                    /* Soft kick ghosts — never on locked 4OTF / one-drop / punk / trap grammar styles. */
                    if (lift && !((kick_m & bit)) && (s == 10u || s == 14u) && h2 < (uint8_t)(48u + live / 4u)
                        && sty != TACK_STYLE_REGGAE
                        && sty != TACK_STYLE_DUB && sty != TACK_STYLE_AMBIENT
                        && sty != TACK_STYLE_BREAKS
                        && sty != TACK_STYLE_JAZZ && sty != TACK_STYLE_JUNGLE
                        && sty != TACK_STYLE_WALTZ && sty != TACK_STYLE_BOSSA && sty != TACK_STYLE_FUNK
                        && sty != TACK_STYLE_BEATLES)
                        hit |= 1u;
                    /* Hats: organic movement — skip Ambient + Breaks (locked grammar). */
                    if (sty != TACK_STYLE_AMBIENT
                        && sty != TACK_STYLE_BREAKS) {
                        if ((s & 1u) && h1 < (uint8_t)(22u + live + dens / 6u))
                            hit |= (1u << 3);
                        if ((s & 3u) == 2u && h5 < (uint8_t)(32u + live / 2u + dens / 5u + (last ? 24u : 0u)))
                            hit |= (1u << 3);
                        if (lift && s == 15u && h2 < (uint8_t)(70u + live + dens / 4u))
                            hit |= (1u << 3);
                        /* Ghost hat dropouts so loops don't feel glued on. */
                        if ((s & 1u) && (hat_m & bit) && h3 < (uint8_t)(12u + ph * 6u) && dens < 70u)
                            hit &= (uint8_t)~(1u << 3);
                    }
                    if ((last || half) && s == 14u && h3 < (uint8_t)(70u + live / 3u)
                       
                        && sty != TACK_STYLE_JAZZ && sty != TACK_STYLE_JUNGLE && sty != TACK_STYLE_REGGAE
                        && sty != TACK_STYLE_WALTZ && sty != TACK_STYLE_BOSSA && sty != TACK_STYLE_FUNK
                        && sty != TACK_STYLE_BEATLES)
                        hit |= 2u;
                    if (dens > 22u && (last || half) && (s == 14u || s == 15u) && h4 < (uint8_t)(130u + live)
                       
                        && sty != TACK_STYLE_BREAKS
                        && sty != TACK_STYLE_FUNK
                        && sty != TACK_STYLE_BOSSA && sty != TACK_STYLE_WALTZ
                        && sty != TACK_STYLE_BEATLES)
                        hit |= (1u << 4);
                    if (sty == TACK_STYLE_BREAKS) {
                        /* Breaks: simple like Jungle — spare kick 0/6/10, 2+4 snare, few hats, air.
                         * Syncopated but readable; not Amen soup; not 4OTF House. */
                        hit = 0;
                        if (s == 0u || s == 6u || s == 10u) hit |= 1u;
                        if (s == 4u || s == 12u) hit |= 2u;
                        if ((s == 2u || s == 8u || s == 14u) && h1 < (uint8_t)(56u + live / 5u))
                            hit |= (1u << 3); /* few closed hats only */
                        /* tiny living: rare ghost hat on lift — no fill spray */
                        if (lift && s == 15u && h2 < (uint8_t)(28u + ph * 4u))
                            hit |= (1u << 3);
                        /* no open / perc — air */
                    }
                    /* JUNGLE FROZEN feel: spare — thin extra hats, no spray. */
                    if (sty == TACK_STYLE_JUNGLE) {
                        if ((s & 1u) && h1 > 70u) hit &= (uint8_t)~(1u << 3);
                        hit &= (uint8_t)~((1u << 6) | (1u << 2));
                    }
                    /* JAZZ FROZEN: light touch only — don't add chaotic fills. */
                    if (sty == TACK_STYLE_JAZZ) {
                        hit &= (uint8_t)~(1u << 6);
                        if (last && dens > 25u && s >= 14u) { /* keep gentle */ }
                        else if (s >= 13u) hit &= (uint8_t)~((1u << 5) | (1u << 2));
                    }
                    /* Beatles: rare soft tom fill on last bar only (no cowbell). */
                    if (sty == TACK_STYLE_BEATLES && last && s >= 12u && h5 < 28u)
                        hit |= (1u << 5);
                    if (sty == TACK_STYLE_REGGAE && (half || last) && s == 8u && h4 < 122u)
                        hit |= (1u << 6);
                    /* End-bar fill spray — skip for locked/minimal/frozen styles. */
                    if (last && dens > 25u && s >= 13u
                       
                        && sty != TACK_STYLE_JAZZ && sty != TACK_STYLE_JUNGLE
                        && sty != TACK_STYLE_REGGAE && sty != TACK_STYLE_AMBIENT
                        && sty != TACK_STYLE_BREAKS
                        && sty != TACK_STYLE_FUNK
                        && sty != TACK_STYLE_BOSSA && sty != TACK_STYLE_WALTZ
                        && sty != TACK_STYLE_BEATLES) {
                        if (s == 13u) hit |= (1u << 5);
                        else if (s == 14u) hit |= 2u;
                        else hit |= (1u << 6);
                    }
                    /* no cowbell / BELL lane (bit7) — ever */
                    if (tack_fill && last
                        && sty != TACK_STYLE_AMBIENT
                       )
                        hit |= (1u << 5) | (1u << 2);
                    hit &= (uint8_t)~(1u << 7);
                } else {
                    /* AMBIENT + FILL: still sparse, but allow soft snare/hat marks */
                    if (s == 0u) hit |= 1u;
                    if (s == 8u && dens > 10u) hit |= 2u;
                    if (last && s == 14u) hit |= (1u << 4);
                    if ((s & 3u) == 2u && h1 < 40u) hit |= (1u << 3);
                }

                hit &= (uint8_t)~(1u << 7); /* hard ban cowbell / BELL lane */
                if (hit) {
                    st->hit = hit;
                    st->time = ST_NOTE;
                    /* Living velocity: phrase/loop micro-dynamics (web-app ghost feel). */
                    {
                        uint8_t base_v = ambient ? 48u : (uint8_t)(s % 4u == 0u ? 108u : 88u + (dens / 8u));
                        uint8_t wob = (uint8_t)(tack_drum_hash((uint8_t)bar, (uint8_t)s, 7) & 15u);
                        if (!ambient) base_v = (uint8_t)(base_v > wob ? base_v - (wob >> 1) : base_v);
                        st->vel = base_v;
                    }
                    st->acc = (uint8_t)((hit & 1u) ? 1u : 0u);
                }
            }
        } /* Style kit */
        } /* 6.10: end !tack_drm_part_own — keep user PART hits */
        else if (tack_fill) {
            /* 6.14 owned-PART fills — overlay phrase-end without stomping grid */
            tack_drm_phrase_fills();
        }
    }
    if (tack_move_rate && tack_move_variant) {
        for (bar = 0; bar < 4u; bar++) {
            const uint32_t base = bar * 16u;
            if (!tack_bar_lit_n[bar]) {
                uint32_t comp_step = bar * tack_bar_steps() + tack_bar_steps() - 2u;
                trk[0].step[comp_step].note[0] = tack_oct_note((int)(60u + tack_state.chords[bar].root));
                trk[0].step[comp_step].n = 1;
                trk[0].step[comp_step].time = ST_NOTE;
                trk[0].step[comp_step].vel = 82;
            }
            trk[1].step[base + 14].note[0] = tack_oct_note((int)(48u + tack_state.chords[bar].root));
            trk[1].step[base + 14].n = 1;
            trk[1].step[base + 14].time = ST_NOTE;
            trk[1].step[base + 14].vel = 92;
            if (!tack_drm_part_own
                && (tack_style_idx != TACK_STYLE_AMBIENT || tack_fill)) {
                trk[3].step[base + 10].hit |= 1u << 2; /* clap only — no cowbell */
                trk[3].step[base + 15].hit |= (1u << 2);
                trk[3].step[base + 10].hit &= (uint8_t)~(1u << 7);
                trk[3].step[base + 15].hit &= (uint8_t)~(1u << 7);
            }
        }
    }
    if (tack_crec_mode == 2u && tack_loop_raw_n) tack_clear(&trk[0]);

}

void tack_app_init(void)
{
    tack_midi_rec_reset();
    memset(tack_bar_lit_n, 0, sizeof tack_bar_lit_n);
    memset(tack_phrase_meta, 0, sizeof tack_phrase_meta);
    memset(tack_undo_meta, 0, sizeof tack_undo_meta);
    {
        extern void (*tack_midi_cc_hook)(uint8_t cc, uint8_t val);
        extern void (*tack_midi_note_hook)(uint8_t channel, uint8_t note, uint8_t vel);
        extern void (*tack_midi_self_hook)(uint8_t note, uint8_t vel);
        tack_midi_cc_hook = tack_midi_cc;
        tack_midi_note_hook = tack_midi_note;
        tack_midi_self_hook = tack_midi_self;
        tack_playback_held_hook = tack_midi_loop_held;
        tack_midi_audio_hook = tack_midi_audio_tick;
    }
    tack_init(&tack_state, 0, TACK_MODE_CANTOPOP, TACK_STYLE_CLASSIC, 0x5441434Bu);
    tack_mode_idx = TACK_MODE_CANTOPOP;
    tack_mute = 0;
    tack_solo = 0;
    tack_mute_sel = 0;
    /* TACK band voices: FM6 pad, FM6 bass, FM6 bell, deep kit. */
    tack_style_idx = TACK_STYLE_CLASSIC;
    tack_octave = 0;
    song.octave = 0;
    tack_fill = 0;
    tack_drm_hats = 1; /* norm */
    tack_drm_spare = 0;
    tack_kick_lock = 0;
    tack_drm_sel = 0;
    tack_drm_depth = 0; /* land on PART */
    tack_drm_part_own = 0; /* style seeds until PART edit/open */
    tack_rand_drm = 0; /* off until user fires GEN */
    tack_drm_lock = 0; /* all lanes unlocked */
    tack_air_user = 0; /* style owns air until user picks */
    tack_drm_lane = 0;
    tack_drm_step = 0;
    tack_arp_last = 1; /* UP */
    tack_beat_kit = 0;
    tack_kit_sel = 0xFFu; /* style owns drums until user picks a kit */
    tack_pick_slot = 0;
    tack_user_chose = 0;
    tack_user_chord.root = 0;
    tack_user_chord.quality = 0;
    {
        uint8_t zi;
        for (zi = 0; zi < TACK_SLOTS; zi++) tack_slot_owned[zi] = 0;
    }
    tack_mute = 0;
    tack_loop = 0;
    tack_dens = 38;
    tack_page = TACK_PG_HOME;
    tack_chaos = 0; /* nothing random unless user enables on GEN */
    tack_gen_sel = 0;
    tack_move_rate = 0; /* variation off — sparse default */
    tack_move_variant = 0;
    tack_move_phase = 0;
    tack_env_down_ms = 0;
    tack_env_long_done = 0;
    tack_pong = 0;
    tack_shimmer = 0;
    tack_air_freeze = 0;
    tack_air_morph = 1;
    tack_morph_left = 0;
    tack_air_target = 0;
    tack_undo_n = 0;
    tack_seed_base = 1;
    tack_snd_bank = 0;
    tack_snd_pick = 0;
    tack_trk_lib_pick[0] = tack_trk_lib_pick[1] = tack_trk_lib_pick[2] = tack_trk_lib_pick[3] = 0;
    tack_apply_style();
    tack_patterns();
    tack_crec_mode = 0;
    tack_crec_steps_ready = 0;
    tack_drm_ratchet = 0;
    tack_fx_scene = 0;
    tack_fx_scene_lock = 0;
    tack_lane_pan[0] = tack_lane_pan[1] = tack_lane_pan[2] = tack_lane_pan[3] = 0;
    tack_play_down_ms = 0;
    tack_play_long_done = 0;
    tack_q_style = tack_q_snd = tack_q_kit = tack_q_fxsc = tack_q_perf = 0xFFu;
    /* A/B arrange defaults — spare strip A A B A; live A; arrange ON. */
    {
        uint32_t i;
        tack_ab_live = 0;
        tack_arr_on = 1u;
        tack_arr_len = 4u;
        tack_arr_pos = 0;
        tack_arr_edit = 0;
        tack_arr_sel = 0;
        tack_arr[0] = 0; tack_arr[1] = 0; tack_arr[2] = 1; tack_arr[3] = 0;
        for (i = 4; i < TACK_ARR_MAX; i++) tack_arr[i] = (uint8_t)(i & 1u);
        tack_phrase_store(0);
        /* Seed phrase B as a shifted colour take (same mode, +5 roots). */
        for (i = 0; i < TACK_SLOTS; i++) {
            tack_phrase[1][i] = tack_state.chords[i];
            tack_phrase[1][i].root = tack_mod12((int)tack_phrase[1][i].root + 5);
        }
        tack_k3_down_ms = 0; tack_k3_long_done = 0;
        tack_home_down_ms = 0; tack_home_long_done = 0;
        tack_midi_master = 127;
        tack_midi_expr = 127;
        for (i = 0; i < 4u; i++)
            tack_level_base[i] = (uint8_t)tack_level[i];
        /* Performances: empty until first SAVE; P1 auto-seeded in tack_app_start. */
        tack_set_sel = 0; tack_set_live = 0; tack_show_lock = 0;
        tack_save_down_ms = 0; tack_save_long_done = 0;
        tack_k4_lock_down_ms = 0; tack_k4_lock_done = 0; tack_set_browse = 0; tack_more_until = 0;
        for (i = 0; i < TACK_PERF_N; i++) tack_perf[i].used = 0;
    }
    tack_crec_len = 0;
    tack_crec_pos = 0;
    tack_auto_last_step = 0xFFu;
    tack_crec_qnt = 1u; /* Quantize ON by default — musical grid for looper */
    tack_crec_rhy = 0;  /* EVEN — keep captured / equal spacing */
    tack_loop_sel = 0;
    tack_chord_grid = 0;
    tack_chord_grid_armed = 0;
    tack_drawn = 0;
    tack_boot_until = 0;
    tack_boot_painted = 0;
    tack_ready = 1;
}

static void tack_restart(void)
{
    /* Rebuild at the next bar line, never by restarting the transport. */
    tack_pending = 1;
    if (!song.playing) {
        tack_patterns();
        tack_pending = 0;
    }
}

void tack_app_start(void)
{
    /* Factory starters P1..P8 — cold gig in under a minute. Does not touch REC/crec. */
    tack_perf_seed_starters();
    for (uint32_t _li = 0; _li < 16u; _li++) tack_learn_map[_li] = 0xFFu;
    tack_zen_calm = 64u;

    /* 6.16 keep: OLED boot splash — big TACK for ~2s (or until first key). */
    tack_boot_painted = 0;
    tack_boot_until = (fm1_ms ? fm1_ms : 1u) + 2000u;

    seq_start();                    /* Felucca sequencer: four TACK tracks, together */
}

static void tack_flash(const char *msg)
{
    str_cpy(tack_action, msg, sizeof tack_action);
    tack_action_until = fm1_ms + 1200u;
    tack_action_serial++;
}

static void tack_flash2(const char *a, const char *b)
{
    str_cpy(tack_action, a, sizeof tack_action);
    str_cpy(tack_action + str_len(tack_action), b, sizeof tack_action - str_len(tack_action));
    tack_action_until = fm1_ms + 1200u;
    tack_action_serial++;
}

/* Quantize hand timing to musical 16th grid.
 * qnt=0 soft: prefer beats then &s (allows variation).
 * qnt=1 hard: beats only (0/4/8/12, Waltz 0/4/8). */
static uint8_t tack_grid_snap(uint8_t step16)
{
    static const uint8_t pref4_soft[8] = { 0, 4, 8, 12, 2, 6, 10, 14 };
    static const uint8_t pref3_soft[6] = { 0, 4, 8, 2, 6, 10 };
    static const uint8_t pref4_hard[4] = { 0, 4, 8, 12 };
    static const uint8_t pref3_hard[3] = { 0, 4, 8 };
    uint8_t bs = tack_bar_steps();
    const uint8_t *pref;
    uint8_t npref, best, bd = 16, i;
    step16 = (uint8_t)(step16 % bs);
    if (tack_crec_qnt) {
        pref = (bs == 12u) ? pref3_hard : pref4_hard;
        npref = (bs == 12u) ? 3u : 4u;
    } else {
        pref = (bs == 12u) ? pref3_soft : pref4_soft;
        npref = (bs == 12u) ? 6u : 8u;
    }
    best = pref[0];
    for (i = 0; i < npref; i++) {
        uint8_t d = (uint8_t)((step16 >= pref[i]) ? (step16 - pref[i]) : (pref[i] - step16));
        if (d > (bs / 2u)) d = (uint8_t)(bs - d);
        if (d < bd) { bd = d; best = pref[i]; }
    }
    return best;
}

/* Re-snap entire phrase after Quantize toggle — keeps chord order. */

/* ===== 5.8 MIDI learn + Zen helpers ===== */
static const char *const TACK_LEARN_TGT[8] = {
    "VOL", "EXPR", "FX1", "FX2", "DENS", "MORPH", "CHAOS", "SPACE"
};
static void tack_learn_apply_cc(uint8_t cc, uint8_t val) {
    uint8_t t;
    tack_learn_last_cc = cc;
    tack_learn_last_val = val;
    if (tack_learn_armed) {
        tack_learn_map[tack_learn_target % 8u] = cc;
        tack_learn_armed = 0u;
        tack_flash("LEARNED");
        return;
    }
    for (t = 0; t < 8u; t++) {
        if (tack_learn_map[t] == cc) {
            switch (t) {
            case 0: /* VOL — master scale all lanes */ {
                uint32_t i;
                tack_midi_master = val;
                for (i = 0; i < 4u; i++)
                    if (tack_level_base[i] == 0 && tack_level[i])
                        tack_level_base[i] = (uint8_t)tack_level[i];
                tack_midi_apply_levels();
                break;
            }
            case 1: /* EXPR — expression scale */ {
                tack_midi_expr = val ? val : 1u;
                tack_midi_apply_levels();
                break;
            }
            case 2: /* FX1 — delay wet */ {
                tack_fx_dly = val;
                tack_apply_fx_amounts();
                break;
            }
            case 3: /* FX2 — reverb wet */ {
                tack_fx_rev = val;
                tack_apply_fx_amounts();
                break;
            }
            case 4: /* DENS — zen calm, else generative density */ {
                if (tack_zen_mode) { tack_zen_calm = val; tack_zen_apply(); }
                else {
                    tack_dens = (uint8_t)(8u + ((uint16_t)val * 56u) / 127u); /* 8..64 */
                    if (tack_dens > 64u) tack_dens = 64u;
                }
                break;
            }
            case 5: /* MORPH — zen calm / air morph amount */ {
                if (tack_zen_mode) { tack_zen_calm = val; tack_zen_apply(); }
                else {
                    tack_fx_vcho = val; /* voice chorus morph */
                    tack_apply_voice_inserts();
                }
                break;
            }
            case 6: /* CHAOS — CC fader: ≥64 on, else off (matches live GEN feel) */ {
                uint8_t on = (val >= 64u) ? 1u : 0u;
                if (on != tack_chaos) {
                    tack_chaos = on;
                    tack_flash(tack_chaos ? "CHAOS ON" : "CHAOS OFF");
                }
                break;
            }
            case 7: /* SPACE — shimmer amount 0..3 */ {
                tack_shimmer = (uint8_t)(val / 43u);
                if (val >= 120u) tack_shimmer = 3u;
                tack_apply_shimmer();
                break;
            }
            default: break;
            }
            return;
        }
    }
}
/* 6.2 Zen morph — one calm knob → voice filter CUT + reverb send + level together. */
static void tack_zen_apply(void)
{
    uint8_t c = tack_zen_calm > 127u ? 127u : tack_zen_calm;
    /* calm↑ = darker filter, more reverb, slightly quieter (pad space). */
    uint8_t cut = (uint8_t)(72u - ((uint16_t)c * 48u) / 127u);   /* 72..24 */
    uint8_t rev = (uint8_t)(40u + ((uint16_t)c * 70u) / 127u);   /* 40..110 */
    uint8_t lvl0 = (uint8_t)(78u - ((uint16_t)c * 22u) / 127u);  /* 78..56 */
    uint8_t lvl1 = (uint8_t)(70u - ((uint16_t)c * 18u) / 127u);
    uint8_t lvl2 = (uint8_t)(64u - ((uint16_t)c * 16u) / 127u);
    uint32_t i;
    for (i = 0; i < 3u; i++) {
        trk[i].p[P_E4] = (int16_t)cut; /* LPF CUT */
        trk[i].p[P_REV] = (int16_t)rev;
    }
    tack_fx_rev = rev;
    tack_apply_fx_amounts();
    /* Soft levels — respect mute/solo gate via tack_level store */
    tack_level[0] = (int16_t)lvl0;
    tack_level[1] = (int16_t)lvl1;
    tack_level[2] = (int16_t)lvl2;
    tack_level_base[0] = lvl0; tack_level_base[1] = lvl1; tack_level_base[2] = lvl2;
    tack_apply_mute_solo();
}

static void tack_zen_enter(void) {
    if (tack_zen_mode) return;
    tack_zen_prev_pg = tack_page;
    tack_zen_mode = 1u;
    tack_chaos = 0u;
    tack_zen_apply();
    tack_open_page(TACK_PG_ZEN);
    tack_flash("ZEN");
}
static void tack_zen_exit(void) {
    if (!tack_zen_mode) return;
    tack_zen_mode = 0u;
    tack_open_page(tack_zen_prev_pg ? tack_zen_prev_pg : TACK_PG_HOME);
    tack_flash("ZEN OFF");
}

/* ===== 5.9 cold/live helpers ===== */
/* 5.9: forward decls for USER phrase load (defs later) */
static void tack_pick_style(uint8_t s);
static void tack_crec_apply_rhythm(void);

static uint8_t tack_learn_destructive_ok(void)
{
    /* Horse-proof: wipe/clear blocked while PLAY or LOCK. */
    if (tack_is_locked()) { tack_flash("LOCKED"); return 0u; }
    if (song.playing) { tack_flash("PLAY"); return 0u; }
    return 1u;
}

static uint8_t tack_user_qual_to_tack(uint8_t q)
{
    /* ingest QUALITY indices → TackQuality */
    static const uint8_t MAP[10] = {
        TACK_Q_MAJ, TACK_Q_MIN, TACK_Q_DIM, TACK_Q_AUG,
        TACK_Q_MAJ7, TACK_Q_MIN7, TACK_Q_DOM7, TACK_Q_M7B5,
        TACK_Q_SUS4, TACK_Q_SUS2
    };
    if (q >= 10u) return TACK_Q_MAJ;
    return MAP[q];
}

static void tack_user_phrase_load(uint8_t idx)
{
    const tack_user_phrase_t *p;
    static const uint8_t MAJ_PC[7] = {0, 2, 4, 5, 7, 9, 11};
    uint8_t i, n;
    if (!TACK_USER_PHRASE_COUNT) {
        tack_flash("NO USER");
        return;
    }
    idx = (uint8_t)(idx % TACK_USER_PHRASE_COUNT);
    p = &TACK_USER_PHRASE[idx];
    tack_user_phrase_sel = idx;
    if (p->style != 0xFFu) {
        uint8_t st = tack_style_migrate(p->style);
        if (st < TACK_STYLE_COUNT) tack_pick_style(st);
    }
    if (p->tempo >= 40u && p->tempo <= 140u)
        song.g[G_BPM] = (int16_t)p->tempo;
    tack_crec_rhy = (uint8_t)(p->rhythm % 7u);
    n = p->nch;
    if (n > TACK_CREC_MAX) n = (uint8_t)TACK_CREC_MAX;
    if (n > TACK_USER_CHORD_MAX) n = TACK_USER_CHORD_MAX;
    tack_midi_rec_reset();
    memset(tack_crec_src, 0, sizeof tack_crec_src);
    memset(tack_crec_lit_n, 0, sizeof tack_crec_lit_n);
    tack_crec_len = n;
    tack_crec_pos = 0;
    tack_crec_mode = 0; /* HELD — LOOP/APPLY opt-in; never auto-loop */
    tack_crec_last_fire = 0xFFu;
    tack_crec_steps_ready = 0;
    for (i = 0; i < n; i++) {
        uint8_t deg = p->deg[i] % 7u;
        uint8_t pc = (uint8_t)((p->root_pc + MAJ_PC[deg]) % 12u);
        tack_crec[i].root = pc;
        tack_crec[i].quality = tack_user_qual_to_tack(p->qual[i]);
        tack_crec_ms[i] = (uint32_t)i * 500u; /* even spacing until APPLY */
        tack_crec_step[i] = (uint8_t)((i * tack_bar_steps()) / (n ? n : 1u));
    }
    if (n) {
        tack_crec_span_ms = tack_crec_ms[n - 1u] + 500u;
        tack_crec_steps_ready = 1u;
    }
    /* Do NOT push into seq / rebind — APPLY and mode are opt-in. */
    tack_flash2("USER ", p->name[0] ? p->name : "HELD");
}

/* 6.2: arm LOOP phrase for Mac export (reverse of inbox ingest). */
static void tack_export_phrase_arm(void)
{
    uint8_t i, n;
    if (!tack_crec_len) {
        tack_flash("NO PHRASE");
        tack_ex_armed = 0;
        return;
    }
    n = tack_crec_len;
    if (n > TACK_CREC_MAX) n = (uint8_t)TACK_CREC_MAX;
    tack_ex_len = n;
    tack_ex_rhy = tack_crec_rhy % 7u;
    tack_ex_root = tack_state.root % 12u;
    tack_ex_style = tack_style_idx;
    tack_ex_tempo = (uint16_t)song.g[G_BPM];
    for (i = 0; i < n; i++) tack_ex[i] = tack_crec[i];
    tack_ex_armed = 1u;
    tack_flash("EXPORT OK");
}

/* Editor SysEx 82 — packed: tempo_lo tempo_hi style rhy root nch then n*(root,qual). */
int tack_ed_phrase_export(uint8_t *out, uint32_t cap)
{
    uint32_t i, o = 0;
    if (!out || cap < 8u) return 0;
    if (!tack_ex_armed || !tack_ex_len) return 0;
    out[o++] = (uint8_t)(tack_ex_tempo & 0x7Fu);
    out[o++] = (uint8_t)((tack_ex_tempo >> 7) & 0x7Fu);
    out[o++] = (uint8_t)(tack_ex_style & 0x7Fu);
    out[o++] = (uint8_t)(tack_ex_rhy & 0x7Fu);
    out[o++] = (uint8_t)(tack_ex_root & 0x7Fu);
    out[o++] = (uint8_t)(tack_ex_len & 0x7Fu);
    for (i = 0; i < tack_ex_len && o + 2u <= cap; i++) {
        out[o++] = (uint8_t)(tack_ex[i].root % 12u);
        out[o++] = (uint8_t)(tack_ex[i].quality & 0x7Fu);
    }
    return (int)o;
}


static void tack_crec_ms_to_steps(void);
static void tack_crec_apply_to_seq(void);
static void tack_crec_loop_tap(void);
static uint8_t tack_bar_steps(void);

static void tack_crec_requantize(void)
{
    uint8_t i;
    if (!tack_crec_steps_ready) tack_crec_ms_to_steps();
    for (i = 0; i < tack_crec_len; i++)
        tack_crec_step[i] = tack_grid_snap(tack_crec_step[i]);
    tack_crec_steps_ready = 1u;
}

/* Chord rhythm templates — in-bar 16ths (4/4). Waltz uses first 3 slots only.
 * EVEN keeps captured timing (or equal-spread on apply). Others rewrite steps. */
static const char *const TACK_RHY_NAME[7] = {
    "EVEN", "WHOLE", "HALF", "QTR", "SYNC", "OFF", "SPARSE"
};
/* Preferred landings per rhythm (0-terminated within 8). */
static const uint8_t TACK_RHY_LAND[7][8] = {
    { 0, 4, 8, 12, 2, 6, 10, 14 }, /* EVEN soft grid — equal-ish */
    { 0, 0, 0, 0, 0, 0, 0, 0 },    /* WHOLE — beat 1 only (repeat) */
    { 0, 8, 0, 8, 0, 8, 0, 8 },    /* HALF — 1+3 */
    { 0, 4, 8, 12, 0, 4, 8, 12 },  /* QTR — all beats */
    { 6, 8, 14, 0, 6, 8, 14, 0 },  /* SYNC — &2 / 3 / &4 */
    { 2, 6, 10, 14, 2, 6, 10, 14 },/* OFFBEAT — &s */
    { 0, 10, 0, 10, 0, 10, 0, 10 } /* SPARSE — 1 + &3 */
};
static const uint8_t TACK_RHY_LAND3[7][4] = { /* Waltz 3/4 */
    { 0, 4, 8, 2 },
    { 0, 0, 0, 0 },
    { 0, 8, 0, 8 },
    { 0, 4, 8, 0 },
    { 4, 8, 2, 4 },
    { 2, 6, 10, 2 },
    { 0, 8, 0, 8 }
};

/* Apply rhythm template to phrase steps — keeps chord order, bar-sync friendly. */
static void tack_crec_apply_rhythm(void)
{
    uint8_t i, bs = tack_bar_steps();
    uint8_t rhy = (uint8_t)(tack_crec_rhy % 7u);
    if (!tack_crec_len) return;
    if (rhy == 0u) {
        /* EVEN: equal spread across bar for N hits (musical, not glued). */
        for (i = 0; i < tack_crec_len; i++) {
            uint8_t st = (uint8_t)((i * bs) / (tack_crec_len ? tack_crec_len : 1u));
            tack_crec_step[i] = tack_grid_snap(st);
        }
        return;
    }
    for (i = 0; i < tack_crec_len; i++) {
        uint8_t st = (bs == 12u) ? TACK_RHY_LAND3[rhy][i & 3u] : TACK_RHY_LAND[rhy][i & 7u];
        if (st >= bs) st = (uint8_t)(st % bs);
        tack_crec_step[i] = tack_crec_qnt ? tack_grid_snap(st) : st;
    }
}

/* Capture landing biased by rhythm when armed. */
static uint8_t tack_crec_rhy_land(uint8_t step16)
{
    uint8_t rhy = (uint8_t)(tack_crec_rhy % 7u);
    uint8_t bs = tack_bar_steps();
    uint8_t best, bd = 16, i, n;
    const uint8_t *lands;
    step16 = (uint8_t)(step16 % bs);
    if (rhy == 0u) return tack_grid_snap(step16);
    if (bs == 12u) { lands = TACK_RHY_LAND3[rhy]; n = 4u; }
    else { lands = TACK_RHY_LAND[rhy]; n = 8u; }
    best = lands[0];
    for (i = 0; i < n; i++) {
        uint8_t L = lands[i] % bs;
        uint8_t d = (uint8_t)((step16 >= L) ? (step16 - L) : (L - step16));
        if (d > (bs / 2u)) d = (uint8_t)(bs - d);
        if (d < bd) { bd = d; best = L; }
    }
    return best;
}

/* Rehabilitate Style/genre + chord choice so picks stay genre-aware
 * (bass/drums/Auto landings). Does NOT wipe user_chose / REC phrase. */
static void tack_style_rebind(void)
{
    tack_set_style_keep(&tack_state, tack_style_idx, tack_slot_owned);
    if (tack_style_idx == TACK_STYLE_BEATLES) {
        tack_mode_idx = TACK_MODE_MAJOR;
        tack_set_mode_keep(&tack_state, TACK_MODE_MAJOR, tack_slot_owned);
    } else {
        tack_set_mode_keep(&tack_state, tack_mode_idx, tack_slot_owned);
    }
    tack_patterns();
}

/* Living Auto grid targets — cycles musical landings, not always bar-1. */
static uint8_t tack_auto_next_grid(void)
{
    /* 4/4: beat1 / &2 / beat3 / &4. 3/4 Waltz: beat1 / beat2 / beat3 / & of 2. */
    static const uint8_t lands4[4] = { 0, 6, 8, 14 };
    static const uint8_t lands3[4] = { 0, 4, 8, 6 };
    const uint8_t *lands = (tack_bar_steps() == 12u) ? lands3 : lands4;
    return lands[(tack_loop + tack_crec_pos + tack_sel) & 3u];
}

/* Bar length in 16ths: Waltz = 12 (true 3/4); others = 16 (4/4). */
static uint8_t tack_bar_steps(void)
{
    return (tack_style_idx == TACK_STYLE_WALTZ) ? 12u : 16u;
}

static uint8_t tack_loop_steps(void)
{
    /* 4 chord bars × steps-per-bar (Waltz 48, else 64). */
    return (uint8_t)(4u * tack_bar_steps());
}

static uint8_t tack_seq_step16(void)
{
    uint8_t bs = tack_bar_steps();
    return (uint8_t)(trk[0].seq_idx % bs);
}

static uint8_t tack_seq_bar(void)
{
    uint8_t bs = tack_bar_steps();
    return (uint8_t)((trk[0].seq_idx / bs) & 3u);
}

/* REC model (6.0): Touch REC = record. Stop = HELD as played.
 * LOOP / APPLY / QNT / RHY / mode-rebind = opt-in only — never auto after REC. */
static void tack_crec_clear(void)
{
    tack_midi_rec_reset();
    tack_crec_mode = 0;
    tack_crec_len = 0;
    tack_crec_pos = 0;
    tack_crec_last_fire = 0xFFu;
    tack_crec_steps_ready = 0;
    tack_crec_span_ms = 0;
    tack_flash("CHORD CLR");
}

static uint32_t tack_rec_now(void)
{
    return (fm1_ms >= tack_crec_t0) ? (fm1_ms - tack_crec_t0) : 0u;
}

static void tack_fill_scale(TackScale *sc)
{
    const TackModeInfo *mi = tack_mode_info(tack_mode_idx);
    uint8_t i;
    sc->tonic = tack_state.root;
    sc->interval_count = mi->count;
    if (sc->interval_count > 12u) sc->interval_count = 12u;
    for (i = 0; i < sc->interval_count; i++)
        sc->intervals[i] = mi->intervals[i];
}

/* SPSC: the audio ISR is the sole producer; the main loop owns the session.
 * Published indices are aligned words; the UI closes the gate before draining.
 * No live session structure is read/written by both contexts. */
#define TACK_REC_QUEUE 64u
typedef struct { uint32_t ms; uint8_t ch, note, vel; } TackRecQueued;
static TackRecQueued tack_rec_queue[TACK_REC_QUEUE];
static volatile uint32_t tack_rec_qwrite, tack_rec_qread;
static volatile uint8_t tack_rec_accept, tack_rec_queue_full;
static void tack_midi_rec_drain(void)
{
    while (tack_rec_qread != tack_rec_qwrite) {
        TackRecQueued q;
        RING_PUBLISH();
        q = tack_rec_queue[tack_rec_qread % TACK_REC_QUEUE];
        (void)tack_rec_session_channel(&tack_midi_sess, q.ch, q.note, q.vel, q.ms);
        RING_PUBLISH();
        tack_rec_qread++;
    }
    if (tack_rec_queue_full || tack_midi_sess.dropped) tack_rec_failed = 1;
}
/* Playback never calls the MIDI input dispatcher. External loopback has no
 * trustworthy origin tag: do not guess by pitch or swallow external releases. */
void tack_midi_self(uint8_t note, uint8_t vel) { (void)note; (void)vel; }
void tack_midi_note(uint8_t channel, uint8_t note, uint8_t vel)
{
    uint32_t w;
    if (!tack_rec_accept || channel > 15u || note > 127u) return;
    w = tack_rec_qwrite;
    if (w - tack_rec_qread >= TACK_REC_QUEUE) { tack_rec_queue_full = 1; return; }
    tack_rec_queue[w % TACK_REC_QUEUE].ch = channel;
    tack_rec_queue[w % TACK_REC_QUEUE].note = note;
    tack_rec_queue[w % TACK_REC_QUEUE].vel = vel;
    tack_rec_queue[w % TACK_REC_QUEUE].ms = tack_rec_now();
    RING_PUBLISH();
    tack_rec_qwrite = w + 1u;
}

static void tack_midi_loop_stop_audio(void)
{
    int i, j;
    for (i = 0; i < tack_audio_n; i++) {
        if (!tack_loop_active[i]) continue;
        tack_loop_active[i] = 0;
        for (j = 0; j < tack_audio_n; j++)
            if (tack_loop_active[j] && tack_loop_mapped[j] == tack_loop_mapped[i]) break;
        if (j == tack_audio_n && !midi_note_held(&trk[0], tack_loop_mapped[i])
            && !midi_local_held(&trk[0], tack_loop_mapped[i]))
            trk_note_off(&trk[0], tack_loop_mapped[i]);
    }
    tack_loop_tick_valid = 0;
}
/* Main thread publishes only a word-sized request. Audio owns all voice gates,
 * active/mapped identities and its private clip snapshot. On this single-core
 * producer/consumer path an ISR cannot overlap a main-thread payload write. */
static void tack_midi_loop_stop(void)
{
    uint32_t req = tack_loop_request;
    if (req & 1u) { RING_PUBLISH(); tack_loop_request = (req + 2u) & ~1u; }
}
static void tack_midi_loop_tick(void)
{
    if (tack_crec_mode == 2u && song.playing && !tack_hold && tack_loop_raw_n) {
        if (!(tack_loop_request & 1u)) {
            uint32_t req = tack_loop_request;
            RING_PUBLISH(); tack_loop_request = (req + 2u) | 1u;
        }
    } else tack_midi_loop_stop();
}

static int tack_midi_loop_held(const track_t *t, uint32_t pitch)
{
    int i;
    if (t != &trk[0]) return 0;
    for (i = 0; i < tack_audio_n; i++)
        if (tack_loop_active[i] && tack_loop_mapped[i] == pitch) return 1;
    return 0;
}
static void tack_midi_rec_reset(void)
{
    tack_rec_accept = 0;
    RING_PUBLISH();
    tack_midi_loop_stop();
    tack_rec_qread = tack_rec_qwrite;
    tack_rec_queue_full = tack_rec_failed = 0;
    tack_rec_session_init(&tack_midi_sess);
    tack_loop_raw_n = 0;
}
static void tack_midi_audio_tick(void)
{
    uint32_t elapsed, cycle, span;
    int i, j;
    TackScale scale;
    uint32_t request = tack_loop_request;
    if (request != tack_loop_seen) {
        tack_midi_loop_stop_audio();
        tack_loop_seen = request;
        tack_audio_n = 0;
        if (request & 1u) {
            RING_PUBLISH();
            seq_release(&trk[0]); /* audio-owned generated comp yields its lane */
            tack_audio_n = tack_loop_raw_n;
            tack_audio_t0 = tack_crec_loop_t0;
            tack_audio_span = tack_crec_span_ms ? tack_crec_span_ms : 1u;
            for (i = 0; i < tack_audio_n; i++) {
                tack_audio_raw[i] = tack_loop_raw[i];
                tack_audio_kind[i] = tack_loop_kind[i];
            }
        }
    }
    if (!(request & 1u) || !tack_audio_n) return;
    if (!song.playing) { tack_midi_loop_stop_audio(); return; }
    span = tack_audio_span;
    elapsed = fm1_ms - tack_audio_t0;
    cycle = elapsed / span;
    elapsed %= span;
    if (!tack_loop_tick_valid || cycle != tack_loop_cycle) {
        tack_midi_loop_stop_audio();
        for (i = 0; i < tack_audio_n; i++) tack_loop_fired[i] = 0;
        tack_loop_cycle = cycle;
        tack_loop_tick_valid = 1;
    }
    tack_fill_scale(&scale);
    /* Release by the pitch latched at note-on, even after root/mode change. */
    for (i = 0; i < tack_audio_n; i++) {
        if (tack_loop_active[i] && elapsed >= tack_audio_raw[i].off_ms) {
            tack_loop_active[i] = 0;
            for (j = 0; j < tack_audio_n; j++)
                if (tack_loop_active[j] && tack_loop_mapped[j] == tack_loop_mapped[i]) break;
            if (j == tack_audio_n && !midi_note_held(&trk[0], tack_loop_mapped[i])
                && !midi_local_held(&trk[0], tack_loop_mapped[i]))
                trk_note_off(&trk[0], tack_loop_mapped[i]);
        }
    }
    for (i = 0; i < tack_audio_n; i++) {
        uint8_t pitch;
        if (tack_loop_fired[i] || elapsed < tack_audio_raw[i].onset_ms) continue;
        tack_loop_fired[i] = 1;
        /* If a delayed audio block missed the entire note, skip it rather than
         * hanging or inventing a release. Timing is audio-block + millisecond clock (not sample exact). */
        if (elapsed >= tack_audio_raw[i].off_ms) continue;
        pitch = tack_audio_raw[i].pitch;
        if (tack_audio_kind[i] == TACK_EV_MONO) pitch = tack_rec_adapt_pitch(pitch, &scale);
        tack_loop_mapped[i] = pitch;
        if (!tack_midi_loop_held(&trk[0], pitch) && !midi_note_held(&trk[0], pitch)
            && !midi_local_held(&trk[0], pitch)) trk_note_on(&trk[0], pitch, 100u);
        tack_loop_active[i] = 1;
    }
    tack_loop_last_elapsed = elapsed;
}

static void tack_crec_store_token_meta(uint8_t idx)
{
    uint8_t j;
    if (idx >= TACK_CREC_MAX) return;
    tack_crec_src[idx] = 0;
    tack_crec_kind[idx] = (uint8_t)TACK_EV_CHORD;
    tack_crec_lit_n[idx] = 0;
    for (j = 0; j < TACK_REC_MAX_VOICES; j++) tack_crec_lit[idx][j] = 0;
}

static int tack_crec_insert_sorted(uint32_t ms, TackChord c, uint8_t src, uint8_t kind,
                                   const uint8_t *lit, uint8_t ln)
{
    uint8_t i, j;
    if (tack_crec_len >= TACK_CREC_MAX) return -1;
    ms = tack_rec_uniquify_ms(tack_crec_ms, (int)tack_crec_len, ms);
    i = tack_crec_len;
    while (i > 0u && tack_crec_ms[i - 1u] > ms) {
        tack_crec[i] = tack_crec[i - 1u];
        tack_crec_ms[i] = tack_crec_ms[i - 1u];
        tack_crec_step[i] = tack_crec_step[i - 1u];
        tack_crec_src[i] = tack_crec_src[i - 1u];
        tack_crec_kind[i] = tack_crec_kind[i - 1u];
        tack_crec_lit_n[i] = tack_crec_lit_n[i - 1u];
        for (j = 0; j < TACK_REC_MAX_VOICES; j++)
            tack_crec_lit[i][j] = tack_crec_lit[i - 1u][j];
        i--;
    }
    tack_crec[i] = c;
    tack_crec_ms[i] = ms;
    tack_crec_step[i] = 0;
    tack_crec_src[i] = src;
    tack_crec_kind[i] = kind;
    tack_crec_lit_n[i] = ln;
    for (j = 0; j < TACK_REC_MAX_VOICES; j++)
        tack_crec_lit[i][j] = (j < ln && lit) ? lit[j] : 0;
    tack_crec_len = (uint8_t)(tack_crec_len + 1u);
    tack_crec_steps_ready = 0;
    return 0;
}

static void tack_midi_rec_finalize(void)
{
    TackClassifiedEv ev[TACK_REC_MAX_EVENTS];
    int n, i;
    uint32_t now = tack_rec_now();
    tack_midi_rec_drain();
    (void)tack_rec_session_channel_flush(&tack_midi_sess, now ? now : 1u);
    n = tack_rec_classify(tack_midi_sess.raw, tack_midi_sess.nraw,
                          TACK_REC_ONSET_CLUSTER,
                          TACK_REC_DEFAULT_ONSET_WINDOW_MS,
                          ev, TACK_REC_MAX_EVENTS);
    if (n < 0 || tack_midi_sess.dropped) tack_rec_failed = 1;
    /* The comp lane has a hard four-voice ceiling. Reject the whole take,
     * never silently truncate a performed chord or a full mixed timeline. */
    if (n + tack_crec_len > TACK_CREC_MAX) tack_rec_failed = 1;
    for (i = 0; i < n; i++) if (ev[i].n > 4u) tack_rec_failed = 1;
    for (i = 0; i < tack_midi_sess.nraw; i++) {
        int j, concurrent = 0;
        for (j = 0; j < tack_midi_sess.nraw; j++)
            if (tack_midi_sess.raw[j].onset_ms <= tack_midi_sess.raw[i].onset_ms
                && tack_midi_sess.raw[j].off_ms > tack_midi_sess.raw[i].onset_ms) concurrent++;
        if (concurrent > 4) tack_rec_failed = 1;
    }
    tack_loop_raw_n = 0;
    if (tack_rec_failed) { tack_crec_len = 0; tack_rec_session_init(&tack_midi_sess); return; }
    tack_loop_raw_n = (uint8_t)tack_midi_sess.nraw;
    for (i = 0; i < tack_loop_raw_n; i++) {
        tack_loop_raw[i] = tack_midi_sess.raw[i];
        tack_loop_ch[i] = tack_midi_sess.raw_channel[i];
        tack_loop_kind[i] = TACK_EV_MONO;

    }
    for (i = 0; i < n; i++) {
        int j;
        for (j = 0; j < tack_loop_raw_n; j++)
            if (ev[i].members & ((uint32_t)1u << j)) tack_loop_kind[j] = ev[i].kind;
    }
    for (i = 0; i < n; i++) {
        TackChord c;
        uint8_t ln = ev[i].n;
        if (ln > TACK_REC_MAX_VOICES) ln = TACK_REC_MAX_VOICES;
        if (ln == 0u) continue;
        c.root = (uint8_t)(ev[i].pitches[0] % 12u);
        c.quality = (ev[i].kind == TACK_EV_CHORD)
            ? tack_kb_quality(ev[i].pitches, ln)
            : TACK_Q_MAJ;
        if (tack_crec_insert_sorted(ev[i].onset_ms, c, 1u, (uint8_t)ev[i].kind,
                                    ev[i].pitches, ln) < 0)
            break;
    }
    tack_rec_session_init(&tack_midi_sess);
}

static void tack_bar_apply_literal(uint8_t bar, uint8_t idx)
{
    uint8_t k, n;
    if (bar >= 4u) return;
    if (idx >= tack_crec_len || !tack_crec_src[idx] || !tack_crec_lit_n[idx]) {
        tack_bar_lit_n[bar] = 0;
        return;
    }
    n = tack_crec_lit_n[idx];
    if (n > 4u) { tack_bar_lit_n[bar] = 0; tack_rec_failed = 1; tack_flash("POLY LIMIT"); return; }
    tack_bar_lit_n[bar] = n;
    tack_bar_lit_kind[bar] = tack_crec_kind[idx];
    for (k = 0; k < n; k++) tack_bar_lit[bar][k] = tack_crec_lit[idx][k];
}

/* Free-capture: chord + wall-clock ms. NOT seq-step / NOT rhythm-land / NOT qnt. */
static void tack_crec_capture(const TackChord *c)
{
    uint32_t ms;
    if (tack_crec_mode != 1u || !c) return;
    if (tack_crec_len >= TACK_CREC_MAX) { tack_rec_failed = 1; return; }
    ms = (fm1_ms >= tack_crec_t0) ? (fm1_ms - tack_crec_t0) : 0u;
    ms = tack_rec_uniquify_ms(tack_crec_ms, (int)tack_crec_len, ms);
    /* Debounce: same chord within ~90ms = one hit. */
    if (tack_crec_len) {
        uint8_t i = (uint8_t)(tack_crec_len - 1u);
        if (tack_crec[i].root == c->root && tack_crec[i].quality == c->quality
            && (ms - tack_crec_ms[i]) < 90u)
            return;
    }
    tack_crec[tack_crec_len] = *c;
    tack_crec_ms[tack_crec_len] = ms;
    tack_crec_step[tack_crec_len] = 0; /* steps filled only on APPLY/QNT/RHY */
    tack_crec_store_token_meta(tack_crec_len);
    tack_crec_len = (uint8_t)(tack_crec_len + 1u);
    tack_crec_steps_ready = 0;
}

/* Derive in-bar steps from free ms — post-OK only (APPLY / QNT / RHY). */
static void tack_crec_ms_to_steps(void)
{
    uint8_t i, bs = tack_bar_steps();
    uint32_t span, beat_ms;
    if (!tack_crec_len) return;
    span = tack_crec_ms[tack_crec_len - 1u];
    if (span < 1u) span = 1u;
    /* Map phrase span onto one bar of 16ths (or keep relative spacing). */
    beat_ms = span;
    for (i = 0; i < tack_crec_len; i++) {
        uint32_t st = (tack_crec_ms[i] * (uint32_t)bs) / beat_ms;
        if (st >= bs) st = (uint32_t)(bs - 1u);
        tack_crec_step[i] = (uint8_t)st;
    }
    tack_crec_span_ms = span + 400u;
    tack_crec_steps_ready = 1u;
}

/* Opt-in: push held phrase into the 4-slot sequence. Never during REC. */
static void tack_crec_apply_to_seq(void)
{
    uint8_t i, n;
    if (tack_crec_mode == 1u) { tack_flash("STOP REC"); return; }
    if (!tack_crec_len) { tack_flash("NO PHRASE"); return; }
    if (!tack_crec_steps_ready) tack_crec_ms_to_steps();
    if (tack_crec_qnt) tack_crec_requantize();
    n = tack_crec_len > TACK_SLOTS ? TACK_SLOTS : tack_crec_len;
    for (i = 0; i < TACK_SLOTS; i++) {
        if (i < n) {
            tack_state.chords[i] = tack_crec[i];
            tack_bar_apply_literal(i, i);
            tack_slot_owned[i] = 1u;
        } else {
            tack_bar_lit_n[i] = 0;
        }
    }
    tack_phrase_store(tack_ab_live);
    tack_pending = 1;
    tack_flash("APPLIED");
}

/* Opt-in LOOP of held phrase — free-time playback, not forced after REC. */
static void tack_crec_loop_tap(void)
{
    if (!tack_crec_len) { tack_flash("NO PHRASE"); return; }
    if (tack_crec_mode == 1u) { tack_flash("STOP REC"); return; }
    if (tack_crec_mode == 2u) {
        tack_crec_mode = 0;
        tack_midi_loop_stop();
        tack_restart();
        tack_flash("LOOP OFF");
        return;
    }
    if (!tack_crec_steps_ready) tack_crec_ms_to_steps();
    if (tack_crec_span_ms < 200u)
        tack_crec_span_ms = tack_crec_ms[tack_crec_len - 1u] + 400u;
    /* ms_to_steps is token-oriented and may shorten span to the last onset.
     * Performed LOOP must include the last individual release before wrapping. */
    {
        uint8_t ri;
        for (ri = 0; ri < tack_loop_raw_n; ri++)
            if (tack_loop_raw[ri].off_ms + 400u > tack_crec_span_ms)
                tack_crec_span_ms = tack_loop_raw[ri].off_ms + 400u;
    }
    tack_crec_mode = 2u;
    tack_crec_pos = 0;
    tack_crec_last_fire = 0xFFu;
    tack_crec_loop_t0 = fm1_ms ? fm1_ms : 1u;
    if (tack_loop_raw_n) {
        tack_patterns();
    }
    if (!song.playing) seq_start(); /* hear the phrase — user asked for LOOP */
    tack_flash("LOOP ON");
}

/* Physical / row REC: arm or stop to HELD. Never auto-LOOP. Never APPLY. */
static void tack_crec_tap(void)
{
    /* Never auto-LOOP. Never APPLY. Stop → HELD as played only. */
    if (tack_crec_mode == 1u) {
        /* Stop REC → HELD as played. Fold MIDI session into the phrase. */
        tack_rec_accept = 0;
        RING_PUBLISH();
        tack_crec_mode = 0;
        tack_midi_rec_finalize();
        tack_crec_pos = 0;
        tack_crec_last_fire = 0xFFu;
        if (tack_crec_len) {
            uint8_t ri;
            tack_crec_span_ms = tack_crec_ms[tack_crec_len - 1u] + 400u;
            for (ri = 0; ri < tack_loop_raw_n; ri++)
                if (tack_loop_raw[ri].off_ms + 400u > tack_crec_span_ms)
                    tack_crec_span_ms = tack_loop_raw[ri].off_ms + 400u;
            tack_flash("CHORD OK");
        } else {
            tack_flash(tack_rec_failed ? "REC FULL" : "CHORD OFF");
        }
        return;
    }
    if (tack_crec_mode == 2u) {
        /* REC while looping: stop loop first, stay HELD. */
        tack_crec_mode = 0;
        tack_midi_loop_stop();
        tack_restart();
        tack_flash("LOOP OFF");
        return;
    }
    /* Off/HELD → arm REC (new take). No seq_start. No auto anything. */
    tack_midi_rec_reset();
    tack_crec_mode = 0;
    tack_crec_len = 0;
    tack_crec_pos = 0;
    tack_crec_steps_ready = 0;
    tack_crec_span_ms = 0;
    tack_crec_last_fire = 0xFFu;
    tack_crec_t0 = fm1_ms ? fm1_ms : 1u;
    tack_rec_qread = tack_rec_qwrite; /* discard any prior generation */
    tack_rec_queue_full = tack_rec_failed = 0;
    tack_rec_session_init(&tack_midi_sess);
    RING_PUBLISH();
    tack_crec_mode = 1u;
    tack_rec_accept = 1;
    tack_flash("CHORD REC");
}

static void tack_push_undo(void);

/* Apply move N to the pick slot, then advance focus to the next slot (wrap). */
static void tack_pick_apply(uint8_t move_idx)
{
    TackChord mv[TACK_MOVES];
    /* 6.2: always the focused/first slot, then push pick_slot to the next. */
    uint8_t slot = (uint8_t)(tack_pick_slot % TACK_SLOTS);
    uint8_t prev = (uint8_t)((slot + TACK_SLOTS - 1u) % TACK_SLOTS);
    const char *tag;
    if (move_idx >= TACK_MOVES) move_idx = 0;
    tack_push_undo();
    /* Moves = ways out of the previous chord into this slot. */
    tack_state.focus = prev;
    tack_next_moves(&tack_state, mv);
    tack_sel = move_idx;
    tack_bar_lit_n[slot] = 0; /* explicit move replaces literal ownership */
    tack_state.chords[slot] = mv[move_idx];
    tack_slot_owned[slot] = 1u;
    tack_state.focus = slot;
    /* Remember manual start — Auto will lead with this chord if enabled later. */
    tack_user_chord = tack_state.chords[slot];
    tack_user_chose = 1u;
    tag = tack_chord_tag(&tack_state.chords[slot]);
    /* REC: free-capture only — audition chord, no seq couple, no rebind, no APPLY. */
    if (tack_crec_mode == 1u) {
        tack_crec_capture(&tack_state.chords[slot]);
        tack_pick_slot = (uint8_t)((slot + 1u) % TACK_SLOTS);
        {
            char msg[20];
            msg[0] = (char)('1' + (char)move_idx);
            msg[1] = ' ';
            str_cpy(msg + 2, tag, sizeof msg - 2u);
            tack_flash(msg);
        }
        return;
    }
    /* Opt-in mode/style rebind — only when NOT recording. */
    tack_style_rebind();
    tack_pick_slot = (uint8_t)((slot + 1u) % TACK_SLOTS);
    /* LOOP edit (opt-in): rewrite last sounded phrase chord — still not seq APPLY. */
    if (tack_crec_mode == 2u && tack_crec_len > 0u) {
        uint8_t i = (uint8_t)(tack_crec_pos % tack_crec_len);
        if (i > 0) i = (uint8_t)(i - 1u);
        tack_crec_store_token_meta(i);
        tack_crec[i] = tack_state.chords[slot];
        tack_crec_ms[i] = (fm1_ms >= tack_crec_loop_t0) ? (fm1_ms - tack_crec_loop_t0) : 0u;
        tack_crec_steps_ready = 0;
    }
    tack_restart();
    tack_phrase_store(tack_ab_live);
    /* Minimal toast: "3 TENSION" — selection + role, no flicker spam. */
    {
        char msg[20];
        msg[0] = (char)('1' + (char)move_idx);
        msg[1] = ' ';
        str_cpy(msg + 2, tag, sizeof msg - 2u);
        tack_flash(msg);
    }
}

/* K2 / SEQ GO — same apply+advance as keys 1–6, using current tack_sel. */
static void tack_commit(void)
{
    tack_pick_apply(tack_sel);
}

static void tack_bridge(void)
{
    tack_bridge_toggle(&tack_state);
    tack_restart();
}

static void tack_next_mode(void)
{
    int16_t keep_bpm = song.g[G_BPM];
    tack_mode_idx = (uint8_t)((tack_mode_idx + 1u) % TACK_MODE_COUNT);
    tack_set_mode_keep(&tack_state, tack_mode_idx, tack_slot_owned);
    /* Re-apply style swing baseline then combo nudges via patterns. */
    tack_apply_style();
    song.g[G_BPM] = keep_bpm; /* mode never stomps tempo */
    if (song.g[G_BPM] > 140) song.g[G_BPM] = 140;
    if (song.g[G_BPM] < 40) song.g[G_BPM] = 40;
    tack_restart();
    tack_flash2("MODE ", TACK_MODE_NAME[tack_mode_idx]);
}


static uint8_t tack_kb_quality(const uint8_t *n, uint8_t cnt)
{
    uint16_t mask = 1u;
    uint8_t i;
    for (i = 1; i < cnt; i++) mask |= (uint16_t)(1u << ((n[i] - n[0]) % 12u));
    if (cnt >= 4u) {
        if (mask == ((1u << 0) | (1u << 4) | (1u << 7) | (1u << 11))) return TACK_Q_MAJ7;
        if (mask == ((1u << 0) | (1u << 4) | (1u << 7) | (1u << 10))) return TACK_Q_DOM7;
        if (mask == ((1u << 0) | (1u << 3) | (1u << 7) | (1u << 10))) return TACK_Q_MIN7;
        if (mask == ((1u << 0) | (1u << 3) | (1u << 6) | (1u << 10))) return TACK_Q_M7B5;
        if (mask == ((1u << 0) | (1u << 3) | (1u << 6) | (1u << 9))) return TACK_Q_DIM7;
    }
    if (mask == ((1u << 0) | (1u << 3) | (1u << 7))) return TACK_Q_MIN;
    if (mask == ((1u << 0) | (1u << 4) | (1u << 7))) return TACK_Q_MAJ;
    if (mask == ((1u << 0) | (1u << 3) | (1u << 6))) return TACK_Q_DIM;
    if (mask == ((1u << 0) | (1u << 4) | (1u << 8))) return TACK_Q_AUG;
    if (mask == ((1u << 0) | (1u << 5) | (1u << 7))) return TACK_Q_SUS4;
    if (mask == ((1u << 0) | (1u << 2) | (1u << 7))) return TACK_Q_SUS2;
    return TACK_Q_CLUSTER;
}


/* Lane labels for Sounds page */
static const char *const TACK_LANE[4] = {"COMP", "BASS", "AIR", "DRUM"};
#define TACK_LIB_N 10u
static const char *const TACK_BANK_NAME[TACK_LIB_N] = {
    "KITS", "AMBIENT", "DARK AMB", "SCI-FI", "303/ACID", "ZEN", "TECHNO", "CLASSIC", "INDUSTRIAL", "JUNO"
};
/* Sounds library accents — distinct per theme (cool, LCD-clear, sparse). */
static const uint16_t TACK_BANK_COL[TACK_LIB_N] = {
    RGB(240, 168, 36),   /* KITS     amber */
    RGB(48, 196, 176),   /* AMBIENT  teal */
    RGB(156, 72, 220),   /* DARK AMB purple — Oberheim huge, not Ambient clone */
    RGB(64, 136, 255),   /* SCI-FI   F/X space blue */
    RGB(96, 224, 56),    /* 303/ACID lime */
    RGB(180, 200, 255),  /* ZEN      soft ice */
    RGB(255, 48, 96),    /* TECHNO   club red */
    RGB(255, 196, 72),   /* CLASSIC  DX7 gold */
    RGB(160, 140, 100),  /* INDUSTRIAL cold steel / foundry */
    RGB(72, 180, 255)    /* JUNO     BBD chorus sky / 80s poly */
};
/* INDUSTRIAL — harsh metals / foundry / scrap / cold drones (≠ Ambient soft / ≠ Techno club / ≠ DX7 Classic) */
#define TACK_IND_N 12u
static const char *const TACK_IND_NAME[TACK_IND_N] = {
    "Scrap Hit", "Foundry", "Cold Drone", "Dist Stab", "Factory Air",
    "Iron Plate", "Pipe Clang", "Rust Saw", "Press Stamp", "Slag Bass",
    "Steam Hiss", "PLANT LIFT~"
};
/* JUNO — Roland Juno-106/60 style poly analog + BBD chorus (≠ Ambient soft / ≠ Techno / ≠ DX7 / ≠ Industrial) */
#define TACK_JUNO_N 12u
static const char *const TACK_JUNO_NAME[TACK_JUNO_N] = {
    "Juno Pad", "Chorus Str", "Soft Brass", "Juno Bass", "Warm Saw",
    "Squ Lead", "Org Choir", "Poly Pluck", "Poly Keys", "PWM Soft",
    "Night Pad", "JUNO LIFT~"
};
/* TECHNO — club techno catalog (not Ambient / not 303 bassline pack) */
#define TACK_TECHNO_N 12u
static const char *const TACK_TECHNO_NAME[TACK_TECHNO_N] = {
    "Hard Saw", "Club Stab", "Acid Hit", "Indust", "Cold Pad",
    "Offbeat", "Pulse Saw", "Warehouse", "Rave Stab", "Noise Tone",
    "Tech Brass", "CLUB LIFT~"
};
/* CLASSIC — Yamaha DX7 / FM catalog (FM6 engine) */
#define TACK_CLASSIC_N 12u
static const char *const TACK_CLASSIC_NAME[TACK_CLASSIC_N] = {
    "Tine EP", "DX Bell", "FM Brass", "FM Bass", "DX Flute",
    "Marimba", "DX Organ", "DX Pluck", "Soft EP", "Brass Stab",
    "Choir Pad", "DX LIFT~"
};
/* Classic pick → FM6 factory preset index */
static const uint8_t TACK_CLASSIC_PRE[TACK_CLASSIC_N] = {
    0, 1, 3, 2, 4, 5, 6, 7, 0, 3, 4, 4
};
/* 303/ACID library sounds — analog presets, no bells */
static const uint8_t TACK_ACID_PRE[6] = {4, 2, 0, 6, 8, 7}; /* ACID SQR SAW RAVE PLUCK SUB */
static const char *const TACK_ACID_NAME[6] = {
    "ACID", "SQR BS", "SAW LD", "RAVE", "PLUCK", "SUB"
};
/* ZEN library — calm subset of ambient named textures (no bells) */
static const uint8_t TACK_ZEN_TEX[8] = {0, 1, 3, 4, 5, 6, 7, 10};
/* Soft Tri Soft Saw Drift HarmBed LowDrone ZenAir QuietBlm HarmPad */
/* Classic kit chips on Sounds (KITS bank) + Drums KIT row */
static const uint16_t TACK_KIT_COL[4] = {
    RGB(255, 120, 40),   /* 808  orange */
    RGB(80, 160, 255),   /* 909  blue */
    RGB(220, 90, 140),   /* CR-78 rose */
    RGB(40, 200, 160)    /* 707  mint */
};
/* Drums page row accents */
static const uint16_t TACK_DRM_COL[10] = {
    RGB(240, 168, 36),   /* KIT */
    RGB(48, 196, 220),   /* DENS cyan */
    RGB(255, 200, 48),   /* HATS gold */
    RGB(80, 140, 255),   /* SPARE blue */
    RGB(255, 88, 72),    /* KICK coral */
    RGB(64, 210, 120),   /* FILL green */
    RGB(220, 120, 255),  /* RATCHET violet */
    RGB(255, 96, 40),    /* CRUSH orange — audible grit */
    RGB(120, 200, 255),  /* COMP ice — bus glue */
    RGB(255, 140, 90)    /* GEN warm */
};
/* Style list accents (Bossa warm coral, Waltz lilac) */
static const uint16_t TACK_STYLE_COL[TACK_STYLE_COUNT] = {
    RGB(200, 200, 200), /* CLASSIC */
    RGB(255, 180, 60),  /* JAZZ */
    RGB(80, 220, 120),  /* JUNGLE */
    RGB(255, 100, 80),  /* REGGAE */
    RGB(100, 180, 200), /* AMBIENT */
    RGB(160, 120, 80),  /* DUB */
    RGB(220, 80, 140),  /* BREAKS */
    RGB(255, 140, 100), /* BOSSA */
    RGB(180, 140, 255), /* WALTZ lilac */
    RGB(255, 200, 40),  /* FUNK */
    RGB(90, 160, 220)   /* BEATLES Mersey blue */
};
static const char *const TACK_PG_TITLE[TACK_PG_COUNT] = {
    "TACK", "LIBRARY", "STYLE", "TRACK SND", "PRESETS", "FX ROOM", "FEEL", "DRUMS", "GEN", "LOOP", "LEARN", "ZEN"
};

static void tack_apply_shimmer(void); /* fwd */
static void tack_apply_pong(void); /* fwd — used by Sci-Fi F/X space */
static void tack_panic(void);
static void tack_crec_live_erase(void);
static void tack_style_rebind(void);
static void tack_crec_requantize(void);
static void tack_crec_apply_rhythm(void);
static void tack_flush_bar_queues(void);
static void tack_queue_style(uint8_t s);
static void tack_queue_snd(uint8_t b);
static void tack_queue_kit(uint8_t k);
static void tack_queue_fxsc(uint8_t sc);
static void tack_queue_perf(uint8_t i);
static void tack_perf_save(uint8_t i);
static void tack_perf_recall(uint8_t i);
static void tack_perf_seed_starters(void);
static uint8_t tack_is_locked(void);
static void tack_phrase_store(uint8_t ab);
static void tack_phrase_load(uint8_t ab);
static void tack_ab_toggle(void);
static void tack_arr_advance(void);
static void tack_midi_apply_levels(void);
void tack_midi_cc(uint8_t cc, uint8_t val); /* USB/TRS controller → TACK */
static void tack_set_lane_level(uint8_t lane, int16_t d);
static void tack_set_lane_pan(uint8_t lane, int16_t d);


static void tack_apply_classic_kit(uint8_t which)
{
    /* which 0..3 → 808 / 909 / CR-78 / 707. Kit only — never touches style or library. */
    which %= 4u;
    set_engine_of(&trk[3], ENGI_DRUM);
    apply_preset_to(&trk[3], TACK_KIT_PRESET[which]);
    tack_kit_sel = which; /* held — style/library apply must re-assert, not clear */
    tack_beat_kit = (which >= 2u) ? (uint8_t)(which - 1u) : 0u; /* CR78→1, 707→2 */
    tack_apply_drum_bus();
    tack_patterns();
    tack_flash2("KIT ", TACK_KIT_TAG[which]);
}

/* Re-assert user kit after style/library so mode/library/kit stay independent. */
static void tack_reassert_kit(void)
{
    if (tack_kit_sel <= 3u)
        tack_apply_classic_kit(tack_kit_sel);
}

static void tack_pick_style(uint8_t s);

static void tack_str_cat(char *d, const char *s, unsigned max)
{
    unsigned n = 0;
    while (n + 1u < max && d[n]) n++;
    while (n + 1u < max && *s) d[n++] = *s++;
    if (max) d[n < max ? n : max - 1u] = 0;
}


static const char *tack_eng_name(uint8_t ei)
{
    ei = (uint8_t)(ei % NENGINES);
    return ENGINES[ei]->name ? ENGINES[ei]->name : "?";
}

static const char *tack_preset_name(track_t *t)
{
    const engine_t *e = ENGINES[t->eng_req % NENGINES];
    if (!e->npresets) return "-";
    return e->presets[t->preset % e->npresets].name;
}

/* Friendly drum kit label (80→808, 10→909). */
static const char *tack_kit_label(track_t *t)
{
    uint8_t k;
    if ((t->eng_req % NENGINES) != ENGI_DRUM) return tack_preset_name(t);
    k = (uint8_t)clamp((int32_t)t->p[P_E0], 0, 11);
    /* Kit model ids (eng_drum file-local): 4=80/808, 5/2=10/909, 6=66/CR-78, 8=77/707. */
    if (k == 4 || t->preset == 4) return "808";
    if (k == 5 || k == 2 || t->preset == 5) return "909";
    if (k == 6 || t->preset == 6) return "CR-78";
    if (k == 8 || t->preset == 7) return "707";
    return tack_preset_name(t);
}

/* 6.8 — kit-aware lane name for PART edit (eng_drum drum_lane_name). */
static const char *tack_drm_lane_name(uint32_t lane)
{
    return drum_lane_name(&trk[3], lane & 7u);
}

/* 6.8 — toggle raw hit on the drum PART for lane at step (all 4 bars). */
static void tack_drm_toggle_step(uint8_t lane, uint8_t step16)
{
    uint8_t bs = tack_bar_steps();
    uint8_t s = (uint8_t)(step16 % (bs ? bs : 16u));
    uint8_t bit = (uint8_t)(1u << (lane & 7u));
    uint8_t bar;
    tack_drm_part_own = 1; /* edits lock PART against style/bar song-list flush */
    for (bar = 0; bar < 4u; bar++) {
        step_t *st = &trk[3].step[(uint32_t)bar * (uint32_t)bs + (uint32_t)s];
        st->hit ^= bit;
        if (st->hit & bit) {
            /* lane turned ON */
            st->time = ST_NOTE;
            if (!st->vel) st->vel = 100u;
            if (bit & 1u) st->acc |= bit; /* kick accent */
        } else {
            /* lane turned OFF — keep other lanes' hits */
            st->acc &= (uint8_t)~bit;
            if (!st->hit) {
                st->time = ST_REST;
                st->vel = 0;
                st->acc = 0;
                st->n = 0;
            }
        }
    }
}

/* 6.8 — read hit bit from bar0 of the PART (display / cursor). */
static uint8_t tack_drm_step_on(uint8_t lane, uint8_t step16)
{
    uint8_t bs = tack_bar_steps();
    uint8_t s = (uint8_t)(step16 % (bs ? bs : 16u));
    return (uint8_t)((trk[3].step[s].hit >> (lane & 7u)) & 1u);
}

/* 6.8 — audible place: fire lane GM note so K2 is never silent. */
static void tack_drm_audition(uint8_t lane)
{
    uint8_t l = lane & 7u;
    trk_note_on(&trk[3], DRUM_LANE_NOTE[l], 110u);
}



/* 6.23 — one STYLE list row via cv_begin/blit (hits OLED; clears old LIVE without list wipe). */
/* 6.27 Soft list craft — AF_M (h19) in ROW_H20: no glyph jump; LIVE = fill+rail+ink, never fat bar. */
#define TACK_LIST_Y    40u
#define TACK_ROW_H     20u
#define TACK_LIST_VIS  7u
#define TACK_SND_VIS    TACK_LIST_VIS
#define TACK_SND_ROW_H  TACK_ROW_H
#define TACK_SND_LIST_Y TACK_LIST_Y

static uint8_t tack_list_top(uint8_t sel, uint8_t n)
{
    if (n <= TACK_LIST_VIS) return 0;
    if (sel < (TACK_LIST_VIS / 2u)) return 0;
    if (sel + (TACK_LIST_VIS / 2u) + 1u > n)
        return (uint8_t)(n - TACK_LIST_VIS);
    return (uint8_t)(sel - (TACK_LIST_VIS / 2u));
}

static void tack_paint_style_row(uint8_t i, uint8_t slot)
{
    char bpm_s[8];
    uint32_t y = TACK_LIST_Y + (uint32_t)slot * TACK_ROW_H;
    uint8_t on = (i == tack_style_cursor);
    uint8_t live = (i == tack_style_idx); /* applied only */
    uint16_t scol = TACK_STYLE_COL[i % TACK_STYLE_COUNT];
    uint16_t sig = TACK_COL_LIVE;
    uint16_t fill = on ? ux_mix(T_BG, sig, 58) : (live ? ux_mix(T_BG, sig, 22) : T_BG);
    uint16_t ink = on ? T_INK : (live ? sig : T_TEXT);
    cv_begin(224u, TACK_ROW_H, fill);
    cv_rect(0, 0, 3, (int32_t)TACK_ROW_H, on ? sig : scol); /* sel = LIVE rail; else style color */
    cv_text_on(8, 2, &AF_M, TACK_STYLE_NAME[i % TACK_STYLE_COUNT], ink, fill);
    fmt_int(bpm_s, TACK_STYLE_BPM[i % TACK_STYLE_COUNT]);
    cv_text_r(216, 3, &AF_S, bpm_s, on ? T_INK : T_DIM, fill); /* BPM only — no fat LIVE badge */
    cv_blit(8, y);
}

/* 6.27 — SOUNDS bank row = STYLE craft (AF_M, no fat LIVE). */
static void tack_paint_snd_lib_row(uint8_t i, uint8_t slot)
{
    uint32_t y = TACK_LIST_Y + (uint32_t)slot * TACK_ROW_H;
    uint8_t on = (i == tack_snd_cursor);
    uint8_t live = (i == (tack_snd_bank % TACK_LIB_N)); /* applied only */
    uint16_t col = TACK_BANK_COL[i % TACK_LIB_N];
    uint16_t sig = TACK_COL_LIVE;
    uint16_t fill = on ? ux_mix(T_BG, sig, 58) : (live ? ux_mix(T_BG, sig, 22) : T_BG);
    uint16_t ink = on ? T_INK : (live ? sig : T_TEXT);
    cv_begin(224u, TACK_ROW_H, fill);
    cv_rect(0, 0, 3, (int32_t)TACK_ROW_H, on ? sig : col);
    cv_text_on(8, 2, &AF_M, TACK_BANK_NAME[i % TACK_LIB_N], ink, fill);
    cv_blit(8, y);
}

static uint8_t tack_snd_live_pick(void)
{
    if ((tack_snd_bank % TACK_LIB_N) == 0u)
        return (uint8_t)(tack_kit_sel % 4u);
    return tack_trk_lib_pick[tack_mute_sel & 3u];
}
static uint8_t tack_snd_list_top(uint8_t n)
{
    return tack_list_top(tack_snd_pick, n);
}
static void tack_paint_snd_preset_row(uint8_t idx, uint8_t slot)
{
    uint8_t bank = (uint8_t)(tack_snd_bank % TACK_LIB_N);
    uint8_t on = (idx == tack_snd_pick);
    uint8_t live = (idx == tack_snd_live_pick());
    uint16_t col = TACK_BANK_COL[bank];
    uint16_t sig = TACK_COL_LIVE;
    uint16_t fill = on ? ux_mix(T_BG, sig, 58) : (live ? ux_mix(T_BG, sig, 22) : T_BG);
    uint16_t ink = on ? T_INK : (live ? sig : T_TEXT);
    uint32_t y = TACK_SND_LIST_Y + (uint32_t)slot * TACK_SND_ROW_H;
    cv_begin(224u, TACK_SND_ROW_H, fill);
    cv_rect(0, 0, 3, (int32_t)TACK_SND_ROW_H, on ? sig : col);
    cv_text_on(8, 2, &AF_M, tack_lib_sound_name(bank, idx), ink, fill);
    cv_blit(8, y);
}

/* 6.10 — ○/● cell into the ACTIVE canvas (caller cv_begin + cv_blit). Never draw orphan cv_*. */
static void tack_drm_circ(int32_t cx, int32_t cy, int32_t r, uint16_t ink, uint16_t under, uint8_t filled)
{
    int32_t d, x, y;
    if (r < 2) r = 2;
    d = r * 2 + 1;
    x = cx - r;
    y = cy - r;
    if (filled) {
        /* disc — rrect when FLAT; solid rect fallback always readable */
        cv_rrect(x, y, d, d, r, ink, under);
        cv_rect(x + 1, y + 1, d - 2, d - 2, ink);
    } else {
        /* hollow ring: frame first (LINE-safe), then soft rrect ring when rounded */
        cv_frame(x, y, d, d, ink);
        if (r >= 3) {
            cv_rrect(x, y, d, d, r, ink, under);
            cv_rrect(x + 1, y + 1, d - 2, d - 2, r - 1, under, under);
        }
    }
}

/* 6.10 — global remake of the whole drum PART (seed + patterns). Only from PART K4 / ARP-again. */

/* 6.14: musical phrase-end fills — works with owned PART too. Ambient stays sparse.
 * Overlays last-bar steps only; never cowbell. FILL off strips the fill-zone extras. */
static void tack_drm_phrase_fills(void)
{
    uint8_t sty = tack_style_idx < TACK_STYLE_COUNT ? tack_style_idx : 0;
    uint8_t ambient = (sty == TACK_STYLE_AMBIENT);
    uint8_t bs = tack_bar_steps();
    uint32_t base = 3u * 16u; /* last bar of 4-bar phrase */
    uint8_t s;
    if (!tack_fill) {
        /* Strip fill-zone extras on last bar (steps 12..bs-1) so FILL OFF is clean. */
        for (s = 12u; s < bs && s < 16u; s++) {
            step_t *st = &trk[3].step[base + s];
            /* Keep kick/snare/hat grammar; drop tom/clap/open/perc spray bits. */
            st->hit &= (uint8_t)~((1u << 2) | (1u << 4) | (1u << 5) | (1u << 6) | (1u << 7));
            if (!st->hit) { st->time = 0; st->vel = 0; }
        }
        return;
    }
    if (ambient) {
        /* Sparse: soft open hat + ghost snare only near phrase end. */
        step_t *st14 = &trk[3].step[base + 14u];
        step_t *st8 = &trk[3].step[base + 8u];
        st14->hit |= (1u << 4);
        st14->time = ST_NOTE;
        if (st14->vel < 40u) st14->vel = 40u;
        if (tack_dens > 10u) {
            st8->hit |= 2u;
            st8->time = ST_NOTE;
            if (st8->vel < 36u) st8->vel = 36u;
        }
        return;
    }
    /* Style-aware last-bar fill — short, musical, not soup. */
    for (s = 12u; s < bs && s < 16u; s++) {
        step_t *st = &trk[3].step[base + s];
        uint8_t h = tack_drum_hash(3u, s, 41u);
        uint8_t add = 0;
        if (sty == TACK_STYLE_JAZZ || sty == TACK_STYLE_BOSSA || sty == TACK_STYLE_WALTZ) {
            if (s == 14u) add |= (1u << 3); /* soft hat */
            if (s == 15u && h < 90u) add |= 2u; /* ghost snare */
        } else if (sty == TACK_STYLE_JUNGLE || sty == TACK_STYLE_BREAKS) {
            if (s == 13u) add |= (1u << 3);
            if (s == 14u) add |= 2u;
            if (s == 15u) add |= (1u << 3);
        } else if (sty == TACK_STYLE_REGGAE || sty == TACK_STYLE_DUB) {
            if (s == 14u) add |= (1u << 4); /* open */
            if (s == 15u && h < 100u) add |= 2u;
        } else if (sty == TACK_STYLE_FUNK || sty == TACK_STYLE_BEATLES) {
            if (s == 13u) add |= (1u << 5); /* tom */
            if (s == 14u) add |= 2u;
            if (s == 15u) add |= (1u << 3);
        } else {
            /* Classic / House-ish: tom → snare → open climb */
            if (s == 12u && h < 100u) add |= (1u << 5);
            if (s == 13u) add |= (1u << 5) | (1u << 2);
            if (s == 14u) add |= 2u | (1u << 3);
            if (s == 15u) add |= (1u << 4) | (1u << 2);
        }
        add &= (uint8_t)~(1u << 7); /* never cowbell */
        if (add) {
            st->hit |= add;
            st->time = ST_NOTE;
            if (st->vel < 70u) st->vel = (uint8_t)(88u + (h & 15u));
            if (st->hit & 1u) st->acc = 1;
        }
    }
}

/* 6.19 PART tools — GEN / REMIX / DENSITY / LOCK.
 * Style guides GEN; user-owned PART stays independent of song-list drums.
 * Locked lanes (tack_drm_lock bit) are never touched by GEN/REMIX/DENSITY. */
static uint8_t tack_drm_lane_locked(uint8_t lane)
{
    return (uint8_t)((tack_drm_lock >> (lane & 7u)) & 1u);
}

static void tack_drm_lock_toggle(void)
{
    uint8_t bit = (uint8_t)(1u << (tack_drm_lane & 7u));
    tack_drm_lock ^= bit;
    if (tack_drm_lock & bit)
        tack_flash2("LOCK ", tack_drm_lane_name(tack_drm_lane));
    else
        tack_flash2("FREE ", tack_drm_lane_name(tack_drm_lane));
}

/* Clear only unlocked lane bits across the 4-bar drum loop (preserve locked). */
static void tack_drm_clear_unlocked(void)
{
    uint8_t bs = tack_bar_steps();
    uint8_t loop_n = (uint8_t)(4u * bs);
    uint8_t s;
    uint8_t keep = tack_drm_lock; /* bits to preserve */
    for (s = 0; s < loop_n; s++) {
        step_t *st = &trk[3].step[s];
        st->hit = (uint8_t)(st->hit & keep);
        st->acc = (uint8_t)(st->acc & keep);
        if (!st->hit) {
            st->time = ST_REST;
            st->vel = 0;
            st->acc = 0;
            st->n = 0;
        }
    }
    trk[3].p[P_SLEN] = loop_n;
    trk[3].p[P_QUANT] = 0;
    trk[3].p[P_CHRD] = 0;
}

/* Style-aware hit plan for one step (unlocked lanes only). */
static uint8_t tack_drm_style_hit(uint8_t sty, uint8_t dens, uint8_t ambient,
                                  uint8_t bar, uint8_t s, uint8_t last)
{
    uint8_t h1 = tack_drum_hash(bar, s, 11);
    uint8_t h2 = tack_drum_hash(bar, s, 22);
    uint8_t h3 = tack_drum_hash(bar, s, 33);
    uint8_t hit = 0;
    if (ambient) {
        if (s == 0u && ((bar & 1u) == 0u || dens > 15u)) hit = 1u;
    } else if (sty == TACK_STYLE_JAZZ) {
        if (s == 0u || s == 8u) hit |= 1u;
        if (s == 4u || s == 12u) hit |= 2u;
        if ((s & 1u) == 0u && h1 < (uint8_t)(40u + dens / 3u)) hit |= (1u << 3);
    } else if (sty == TACK_STYLE_JUNGLE || sty == TACK_STYLE_BREAKS) {
        if (s == 0u || s == 6u || s == 10u) hit |= 1u;
        if (s == 4u || s == 12u) hit |= 2u;
        if ((s == 2u || s == 8u || s == 14u) && h1 < (uint8_t)(50u + dens / 4u)) hit |= (1u << 3);
    } else if (sty == TACK_STYLE_REGGAE || sty == TACK_STYLE_DUB) {
        if (s == 0u) hit |= 1u;
        if (s == 4u || s == 12u) hit |= 2u;
        if ((s & 3u) == 2u && h2 < (uint8_t)(36u + dens / 4u)) hit |= (1u << 3);
    } else if (sty == TACK_STYLE_WALTZ) {
        if (s == 0u || s == 4u || s == 8u) hit |= 1u;
        if ((s == 2u || s == 6u || s == 10u) && h1 < 80u) hit |= (1u << 3);
    } else if (sty == TACK_STYLE_BOSSA || sty == TACK_STYLE_FUNK || sty == TACK_STYLE_BEATLES) {
        if (s == 0u || s == 8u) hit |= 1u;
        if (s == 4u || s == 12u) hit |= 2u;
        if ((s & 3u) == 0u && h1 < (uint8_t)(55u + dens / 3u)) hit |= (1u << 3);
        if (s == 6u && h2 < (uint8_t)(30u + dens / 5u)) hit |= 1u;
    } else {
        if (s == 0u || s == 8u) hit |= 1u;
        if ((s == 4u || s == 12u) && dens > 8u) hit |= 2u;
        if (h3 < (uint8_t)(18u + dens / 2u) && (s & 1u) == 0u) hit |= (1u << 3);
        if (dens > 40u && s == 10u && h2 < 60u) hit |= 1u;
        if (dens > 55u && (s == 2u || s == 14u) && h1 < 50u) hit |= 2u;
    }
    if (!ambient && last && dens > 30u && s >= 12u && h3 < 40u)
        hit |= (1u << 3);
    hit &= (uint8_t)~(1u << 7);
    /* Mask out locked lanes — never propose hits there. */
    hit = (uint8_t)(hit & (uint8_t)~tack_drm_lock);
    return hit;
}

/* GEN — generate a usable style-aware groove into owned PART (unlocked lanes). */
static void tack_drm_gen(void)
{
    uint8_t sty = tack_style_idx < TACK_STYLE_COUNT ? tack_style_idx : 0;
    uint8_t dens = tack_dens > 100u ? 100u : tack_dens;
    uint8_t ambient = (sty == TACK_STYLE_AMBIENT);
    uint8_t bs = tack_bar_steps();
    uint8_t bar, s;
    uint32_t seed = tack_state.seed ^ ((uint32_t)fm1_ms * 0x9E3779B1u) ^ 0x47454E21u; /* 'GEN!' */
    tack_push_undo();
    tack_seed_base ^= seed | 1u;
    tack_state.seed ^= tack_seed_base | 1u;
    tack_loop++;
    tack_drm_clear_unlocked();
    for (bar = 0; bar < 4u; bar++) {
        const uint32_t base = (uint32_t)bar * 16u;
        const uint8_t last = (bar == 3u);
        for (s = 0; s < bs; s++) {
            step_t *st = &trk[3].step[base + s];
            uint8_t hit = tack_drm_style_hit(sty, dens, ambient, bar, s, last);
            if (hit) {
                st->hit = (uint8_t)((st->hit & tack_drm_lock) | hit);
                st->time = ST_NOTE;
                st->vel = ambient ? 44u : (uint8_t)(s % 4u == 0u ? 110u : 90u + (dens / 8u));
                st->acc = (uint8_t)((st->acc & tack_drm_lock) | ((hit & 1u) ? 1u : 0u));
            }
        }
    }
    tack_drm_part_own = 1;
    tack_rand_drm = 1;
    if (tack_fill) tack_drm_phrase_fills();
    tack_restart();
    tack_flash("GEN");
}

/* 6.14/6.19 alias — FEEL ROLL path + any leftover call sites. */
static void tack_rand_drm_fire(void)
{
    tack_drm_gen();
}

/* REMIX — variation of current PART; keep character, change unlocked hits. */
static void tack_drm_remix(void)
{
    uint8_t sty = tack_style_idx < TACK_STYLE_COUNT ? tack_style_idx : 0;
    uint8_t dens = tack_dens > 100u ? 100u : tack_dens;
    uint8_t ambient = (sty == TACK_STYLE_AMBIENT);
    uint8_t bs = tack_bar_steps();
    uint8_t bar, s, lane;
    uint32_t seed = tack_state.seed ^ ((uint32_t)fm1_ms * 0x85EBCA6Bu) ^ 0x524D5821u; /* 'RMX!' */
    tack_push_undo();
    tack_seed_base ^= seed | 1u;
    tack_state.seed ^= tack_seed_base | 1u;
    tack_loop++;
    for (bar = 0; bar < 4u; bar++) {
        const uint32_t base = (uint32_t)bar * (uint32_t)bs;
        for (s = 0; s < bs; s++) {
            step_t *st = &trk[3].step[base + s];
            uint8_t h1 = tack_drum_hash((uint8_t)(bar + (uint8_t)(seed & 7u)), s, 41);
            uint8_t h2 = tack_drum_hash((uint8_t)bar, (uint8_t)(s + 3u), 52);
            uint8_t hit = st->hit;
            uint8_t keep = tack_drm_lock;
            uint8_t unlocked = (uint8_t)~keep;
            /* Drop some unlocked hits (keep strong beats more often). */
            for (lane = 0; lane < 8u; lane++) {
                uint8_t bit = (uint8_t)(1u << lane);
                if (!(unlocked & bit)) continue;
                if (hit & bit) {
                    uint8_t drop = 28u;
                    if (lane == 0u && (s == 0u || s == 8u)) drop = 8u; /* keep kick 1+3 */
                    if (lane == 1u && (s == 4u || s == 12u)) drop = 12u;
                    if (h1 < drop) hit = (uint8_t)(hit & (uint8_t)~bit);
                } else if (!ambient && dens > 12u) {
                    /* Sparse add — style ghost, not noise. */
                    uint8_t add = (uint8_t)(10u + dens / 6u);
                    if (lane == 3u && (s & 1u) == 0u && h2 < add)
                        hit |= bit;
                    else if (lane == 0u && (s == 6u || s == 10u) && h1 < (uint8_t)(add / 2u))
                        hit |= bit;
                    else if (lane == 1u && (s == 2u || s == 14u) && dens > 40u && h2 < (uint8_t)(add / 2u))
                        hit |= bit;
                }
            }
            hit = (uint8_t)((hit & unlocked) | (st->hit & keep));
            hit &= (uint8_t)~(1u << 7);
            st->hit = hit;
            if (hit) {
                st->time = ST_NOTE;
                if (!st->vel) st->vel = ambient ? 44u : 96u;
                st->acc = (uint8_t)((st->acc & keep) | ((hit & 1u) ? 1u : 0u));
            } else {
                st->time = ST_REST;
                st->vel = 0;
                st->acc = 0;
                st->n = 0;
            }
            (void)sty;
        }
    }
    tack_drm_part_own = 1;
    if (tack_fill) tack_drm_phrase_fills();
    tack_restart();
    tack_flash("REMIX");
}

/* DENSITY — thin/thicken unlocked PART hits musically (owned grid). */
static void tack_drm_density_nudge(int dir)
{
    uint8_t sty = tack_style_idx < TACK_STYLE_COUNT ? tack_style_idx : 0;
    uint8_t dens = tack_dens > 100u ? 100u : tack_dens;
    uint8_t ambient = (sty == TACK_STYLE_AMBIENT);
    uint8_t bs = tack_bar_steps();
    uint8_t bar, s, lane;
    uint8_t unlocked = (uint8_t)~tack_drm_lock;
    uint32_t seed = tack_state.seed ^ ((uint32_t)fm1_ms * 0xC2B2AE35u) ^ (uint32_t)(dir * 17);
    if (!tack_drm_part_own) {
        /* Not owned yet — claim + seed once so dens has a grid to shape. */
        tack_drm_part_own = 0;
        tack_patterns();
        tack_drm_part_own = 1;
    }
    tack_push_undo();
    for (bar = 0; bar < 4u; bar++) {
        const uint32_t base = (uint32_t)bar * (uint32_t)bs;
        for (s = 0; s < bs; s++) {
            step_t *st = &trk[3].step[base + s];
            uint8_t h = tack_drum_hash((uint8_t)(bar + (uint8_t)(seed & 3u)), s, (uint8_t)(60u + (dir > 0 ? 1u : 2u)));
            uint8_t hit = st->hit;
            if (dir < 0) {
                /* Thin: drop weak unlocked hits; protect kick on 1. */
                for (lane = 0; lane < 8u; lane++) {
                    uint8_t bit = (uint8_t)(1u << lane);
                    if (!(unlocked & bit) || !(hit & bit)) continue;
                    if (lane == 0u && s == 0u) continue;
                    uint8_t chance = (uint8_t)(22u + (100u - dens) / 4u);
                    if ((s & 1u) && chance < 90u) chance = (uint8_t)(chance + 10u);
                    if (h < chance) hit = (uint8_t)(hit & (uint8_t)~bit);
                }
            } else {
                /* Thicken: add style-aware unlocked hits on empty slots. */
                uint8_t add = tack_drm_style_hit(sty, dens, ambient, bar, s, (uint8_t)(bar == 3u));
                add = (uint8_t)(add & unlocked);
                for (lane = 0; lane < 8u; lane++) {
                    uint8_t bit = (uint8_t)(1u << lane);
                    if (!(add & bit) || (hit & bit)) continue;
                    if (h < (uint8_t)(40u + dens / 3u)) hit |= bit;
                }
            }
            hit = (uint8_t)((hit & unlocked) | (st->hit & tack_drm_lock));
            st->hit = hit;
            if (hit) {
                st->time = ST_NOTE;
                if (!st->vel) st->vel = ambient ? 44u : 92u;
                st->acc = (uint8_t)((st->acc & tack_drm_lock) | ((hit & 1u) ? 1u : 0u));
            } else {
                st->time = ST_REST;
                st->vel = 0;
                st->acc = 0;
                st->n = 0;
            }
        }
    }
    tack_drm_part_own = 1;
    tack_restart();
    tack_flash("DENS");
}

static void tack_drm_remake(void)
{
    /* Style-mask reseed (power path). Primary UX is GEN/REMIX. */
    tack_drm_part_own = 0;
    tack_seed_base ^= (uint32_t)fm1_ms * 0x9E3779B1u + 0xA5A5A5A5u;
    tack_state.seed ^= tack_seed_base | 1u;
    tack_loop++;
    tack_patterns();
    tack_drm_part_own = 1;
    tack_restart();
    tack_flash("REMAKE");
}

/* FEEL ARP row still toggles arpeggiator. Physical ARP opens PART (see tack_open_drums_part). */

static uint8_t tack_arp_is_on(void)
{
    track_t *t = &trk[tack_mute_sel & 3u];
    return t->p[P_AMODE] != 0;
}

static void tack_arp_toggle(void)
{
    track_t *t = &trk[tack_mute_sel & 3u];
    if (t->p[P_AMODE]) {
        if (t->p[P_AMODE] > 0)
            tack_arp_last = (uint8_t)t->p[P_AMODE];
        t->p[P_AMODE] = 0;
        tack_flash2("ARP OFF ", TACK_LANE[tack_mute_sel & 3u]);
    } else {
        if (!tack_arp_last) tack_arp_last = 1; /* UP */
        t->p[P_AMODE] = (int16_t)tack_arp_last;
        tack_flash2("ARP ON ", TACK_LANE[tack_mute_sel & 3u]);
    }
}

static void tack_arp_cycle_mode(int dir)
{
    track_t *t = &trk[tack_mute_sel & 3u];
    static const char *const NM[] = {"OFF", "UP", "DN", "UPDN", "RND", "ORD", "REPEAT"};
    int32_t m = (int32_t)t->p[P_AMODE] + (dir > 0 ? 1 : -1);
    if (m < 0) m = 6;
    if (m > 6) m = 0;
    t->p[P_AMODE] = (int16_t)m;
    if (m > 0) tack_arp_last = (uint8_t)m;
    tack_flash2("ARP ", NM[m]);
}

static void tack_flash_bpm(void)
{
    char b[8];
    fmt_int(b, song.g[G_BPM]);
    tack_flash2("BPM ", b);
}

static void tack_go_home(void)
{
    tack_page = TACK_PG_HOME;
    /* Safety: drums must never stay silent after leaving an ambient bank/style. */
    if (!(tack_mute & (1u << 3)) && trk[3].p[P_LEVEL] == 0) {
        if (tack_level[3] < 40) tack_level[3] = 100;
        trk[3].p[P_LEVEL] = tack_level[3];
    }
    tack_flash("HOME");
}

static void tack_open_page(uint8_t pg)
{
    tack_page = pg % TACK_PG_COUNT;
    if (tack_page == TACK_PG_STYLE)
        tack_style_cursor = tack_style_idx;
    if (tack_page == TACK_PG_SOUNDS) {
        tack_snd_cursor = tack_snd_bank % TACK_LIB_N; /* highlight matches live library */
        tack_snd_depth = 0; /* always land on LIBRARY list */
        if (tack_snd_pick >= tack_lib_sound_n(tack_snd_bank))
            tack_snd_pick = 0;
    }
    if (tack_page == TACK_PG_LOOP) {
        if (tack_loop_sel > 4u) tack_loop_sel = 0;
        tack_style_rebind(); /* rehabilitate genre when entering looper */
    }
    if (tack_page == TACK_PG_DRUMS) {
        tack_drm_depth = 0; /* always land on PART step grid */
        if (tack_drm_lane > 7u) tack_drm_lane = 0;
        {
            uint8_t bs = tack_bar_steps();
            if (tack_drm_step >= bs) tack_drm_step = 0;
        }
    }
    tack_flash(TACK_PG_TITLE[tack_page]);
}

/* 6.10 — ARP / entry: open Drums PART (force redraw). Already on PART → remake. Never via GLO. */
static void tack_open_drums_part(void)
{
    if (tack_is_locked()) { tack_flash("LOCKED"); return; }
    if (tack_drm_lane > 7u) tack_drm_lane = 0;
    {
        uint8_t bs = tack_bar_steps();
        if (tack_drm_step >= bs) tack_drm_step = 0;
    }
    if (tack_page == TACK_PG_DRUMS && tack_drm_depth == 0u) {
        /* 6.19 ARP-again on PART → REMIX (keep character, change unlocked hits). */
        tack_drm_remix();
        return;
    }
    /* Open/land PART: keep live hits as seed, claim ownership so bar flush cannot stomp. */
    tack_drm_part_own = 1;
    if (tack_page == TACK_PG_DRUMS) {
        tack_drm_depth = 0; /* FEEL → PART */
        /* 6.12 soft — depth in sig; no full wipe */
        tack_flash("PART");
        return;
    }
    tack_open_page(TACK_PG_DRUMS);
    tack_drm_depth = 0;
    /* 6.12: page change alone drives need_full */
    tack_flash("PART");
}


static void tack_push_undo(void)
{
    uint32_t i, s;
    if (tack_undo_n < 4u) tack_undo_n++;
    for (s = 3u; s > 0u; s--)
        for (i = 0; i < TACK_SLOTS; i++)
            tack_undo[s][i] = tack_undo[s - 1u][i];
    for (s = 3u; s > 0u; s--)
        for (i = 0; i < TACK_SLOTS; i++) tack_undo_meta[s][i] = tack_undo_meta[s-1u][i];
    tack_meta_store(tack_undo_meta[0]);
    for (i = 0; i < TACK_SLOTS; i++)
        tack_undo[0][i] = tack_state.chords[i];
}

static void tack_do_undo(void)
{
    uint32_t i, s;
    if (!tack_undo_n) { tack_flash("NO UNDO"); return; }
    for (i = 0; i < TACK_SLOTS; i++)
        tack_state.chords[i] = tack_undo[0][i];
    tack_meta_load(tack_undo_meta[0]);
    for (s = 0; s + 1u < tack_undo_n; s++)
        for (i = 0; i < TACK_SLOTS; i++) {
            tack_undo[s][i] = tack_undo[s + 1u][i];
            tack_undo_meta[s][i] = tack_undo_meta[s + 1u][i];
        }
    tack_undo_n--;
    tack_phrase_store(tack_ab_live);
    tack_restart();
    tack_flash("UNDO");
}

static void tack_roll_take(void)
{
    tack_push_undo();
    tack_seed_base ^= (uint32_t)fm1_ms * 0x9E3779B1u + 0xA5A5A5A5u;
    tack_state.seed ^= tack_seed_base | 1u;
    tack_loop++;
    tack_patterns();
    tack_restart();
    tack_flash("ROLL");
}

static void tack_apply_shimmer(void)
{
    /* Audible air: chorus + delay on AIR. Ambient styles keep rate slow (sombre, not sparkly).
     * sh==0 must NOT clobber FX-page / style chorus — that made CHORUS row feel dead. */
    uint8_t sh = tack_shimmer;
    uint8_t sombre = (tack_style_idx == TACK_STYLE_AMBIENT ||
                      tack_style_idx == TACK_STYLE_DUB);
    if (!sh) {
        /* Restore masters onto air; leave bus depths as FX page left them. */
        trk[2].p[P_CHOR] = tack_fx_cho;
        return;
    }
    {
        int16_t chor = (int16_t)(28 + sh * (sombre ? 22 : 32));
        int16_t dly = (int16_t)(28 + sh * (sombre ? 26 : 24));
        if (chor > 118) chor = 118;
        if (dly > 110) dly = 110;
        if (chor < (int16_t)tack_fx_cho) chor = (int16_t)tack_fx_cho;
        trk[2].p[P_CHOR] = (uint8_t)chor;
        trk[2].p[P_DLY] = (uint8_t)(dly > (int16_t)trk[2].p[P_DLY] ? dly : trk[2].p[P_DLY]);
        if (trk[2].p[P_REV] < 90) trk[2].p[P_REV] = (uint8_t)(90 + sh * 6);
        song.g[G_CDEPTH] = (int16_t)((sombre ? 62 : 55) + sh * (sombre ? 16 : 24));
        if (song.g[G_CDEPTH] < (int16_t)tack_fx_cho) song.g[G_CDEPTH] = (int16_t)tack_fx_cho;
        song.g[G_CRATE] = (int16_t)(sombre ? (12 + sh * 4) : (28 + sh * 12));
        if (song.g[G_DMIX] < 90) song.g[G_DMIX] = 90 + sh * 4;
        if (sombre && song.g[G_RSIZE] < 110) song.g[G_RSIZE] = 110 + sh * 3;
        if (sombre && song.g[G_RDAMP] > 20) song.g[G_RDAMP] = 18; /* keep room cold */
    }
}

/* Drum bus: P_DIST = character distortion; P_E7 DRV = softclip into soft_knee (glue/punch).
 * Mild IT_SOFT INSERT wet follows COMP for extra bus glue — still not trashy. */
static void tack_apply_drum_bus(void)
{
    /* Cap 100 — engaged DIST/COMP must clearly colour the kit (3.8 hotfix). */
    uint8_t d = tack_drm_dist > 100u ? 100u : tack_drm_dist;
    uint8_t c = tack_drm_comp > 100u ? 100u : tack_drm_comp;
    trk[3].p[P_DIST] = d;
    trk[3].p[P_E7] = c; /* DRV softclip — tames peaks after/with drive */
    trk[3].p[P_LN7] = 0; /* BELL/cowbell lane silent — no cowbell in TACK */
    /* Soft INSERT as bus saturation when COMP is engaged — wet enough to hear */
    if (c >= 8u || d >= 12u) {
        trk[3].p[P_ITYPE] = IT_SOFT;
        trk[3].p[P_IA] = (int16_t)(28 + (c >> 1) + (d >> 3)); /* drive — clearly grit */
        if (trk[3].p[P_IA] > 100) trk[3].p[P_IA] = 100;
        trk[3].p[P_IB] = 66;                          /* tone — darkish */
        trk[3].p[P_IC] = 118;                         /* level */
        trk[3].p[P_IMIX] = (int16_t)(36 + (c * 3) / 4 + (d >> 2)); /* wet glue */
        if (trk[3].p[P_IMIX] > 96) trk[3].p[P_IMIX] = 96;
    } else {
        trk[3].p[P_ITYPE] = IT_OFF;
        trk[3].p[P_IMIX] = 0;
    }
    /* Ambient/Dark: keep SNAP modest so kick stays discreet, not snappy trash */
    if (tack_style_idx == TACK_STYLE_AMBIENT) {
        if (trk[3].p[P_E4] > 48) trk[3].p[P_E4] = 40; /* SNAP */
        if (trk[3].p[P_E5] > 90) trk[3].p[P_E5] = 80; /* ACC */
    }
}


/* 6.15+ voice-path inserts: FLANGER / BIT REDUC / V CHO / CHAOS on COMP/BASS/AIR.
 * Never stomp trk[3] — drum bus owns IT_SOFT (Drums CRUSH stays separate).
 * Amount > 0 must be clearly audible; DRY (all zero) stays truly dry.
 * CHAOS is KAOSS-style combo (not phaser-only): COMP+BASS fold/fuzz grit, AIR phaser,
 * plus CUT↓/RES↑ on 0..2. Alone = intentional split. Several on: crush > chaos > flange > chorus. */
static void tack_apply_tape_delay(void)
{
    /* Short tape: force delay TIME to 1/16..1/8 range and push wet from tack_fx_tape. */
    uint8_t t = tack_fx_tape > 127u ? 127u : tack_fx_tape;
    if (t == 0u) return;
    /* G_DTIME indices: 2=1/16, 1=1/8, 3=1/32 — keep short. */
    if (t < 40u) song.g[G_DTIME] = 3;      /* 1/32 */
    else if (t < 80u) song.g[G_DTIME] = 2; /* 1/16 */
    else song.g[G_DTIME] = 1;              /* 1/8 */
    if (song.g[G_DFDBK] < 28) song.g[G_DFDBK] = 28 + (t >> 3);
    if (song.g[G_DCOLOR] < 50) song.g[G_DCOLOR] = 55;
    {
        uint8_t wet = (uint8_t)(18u + ((uint16_t)t * 90u) / 127u);
        if (tack_fx_dly < wet) {
            tack_fx_dly = wet;
            /* light push — full apply_fx_amounts would recurse; set sends directly */
            trk[0].p[P_DLY] = (uint8_t)((uint16_t)wet * 70u / 100u);
            trk[1].p[P_DLY] = (uint8_t)((uint16_t)wet * 40u / 100u);
            trk[2].p[P_DLY] = wet;
            trk[3].p[P_DLY] = (uint8_t)((uint16_t)wet * 28u / 100u);
            song.g[G_DMIX] = (int16_t)(wet < 12 ? 12 : wet);
        }
    }
}

/* BIT REDUC → IT_CRUSH: IA=BITS (low=more crush), IB=RATE (low=slower hold=grittier), IC=LPF, IMIX=wet.
 * Curve is aggressive so small K2 steps are already hearable on pads/bass. */
static void tack_voice_set_crush(uint8_t slot, uint8_t c)
{
    /* BITS: amount↑ → fewer bits. c=4 → ~10 bit; c=64 → ~5 bit; c=127 → ~2 bit. */
    int16_t bits = (int16_t)(88 - ((uint16_t)c * 80u) / 127u);
    int16_t rate, lpf, mix;
    if (bits < 4) bits = 4;
    /* RATE: amount↑ → slower sample-hold (IB↓). */
    rate = (int16_t)(100 - ((uint16_t)c * 88u) / 127u);
    if (rate < 8) rate = 8;
    lpf = (int16_t)(70 + (c >> 2)); /* keep grit; mild darken at high */
    if (lpf > 110) lpf = 110;
    /* MIX: floor wet so amount>0 is never near-dry. */
    mix = (int16_t)(56 + ((uint16_t)c * 68u) / 127u);
    if (mix > 120) mix = 120;
    trk[slot].p[P_ITYPE] = IT_CRUSH;
    trk[slot].p[P_IA] = bits;
    trk[slot].p[P_IB] = rate;
    trk[slot].p[P_IC] = lpf;
    trk[slot].p[P_IMIX] = mix;
}

/* V CHO → IT_CHORUS: IA=RATE, IB=DEPTH, IC=FDBK, IMIX=wet — deep enough to hear beside send CHO. */
static void tack_voice_set_chorus(uint8_t slot, uint8_t h)
{
    int16_t rate = (int16_t)(24 + (h >> 1));          /* motion from first steps */
    int16_t depth = (int16_t)(48 + ((uint16_t)h * 72u) / 127u);
    int16_t fbk = (int16_t)(36 + (h >> 1));
    int16_t mix = (int16_t)(52 + ((uint16_t)h * 70u) / 127u);
    if (depth > 120) depth = 120;
    if (fbk > 110) fbk = 110;
    if (mix > 120) mix = 120;
    trk[slot].p[P_ITYPE] = IT_CHORUS;
    trk[slot].p[P_IA] = rate;
    trk[slot].p[P_IB] = depth;
    trk[slot].p[P_IC] = fbk;
    trk[slot].p[P_IMIX] = mix;
}

static void tack_voice_set_flange(uint8_t slot, uint8_t f)
{
    int16_t rate = (int16_t)(20 + (f >> 2));
    int16_t depth = (int16_t)(44 + ((uint16_t)f * 72u) / 127u);
    int16_t fbk = (int16_t)(48 + (f >> 1));
    int16_t mix = (int16_t)(48 + ((uint16_t)f * 70u) / 127u);
    if (depth > 120) depth = 120;
    if (fbk > 118) fbk = 118;
    if (mix > 120) mix = 120;
    trk[slot].p[P_ITYPE] = IT_FLANGER;
    trk[slot].p[P_IA] = rate;
    trk[slot].p[P_IB] = depth;
    trk[slot].p[P_IC] = fbk;
    trk[slot].p[P_IMIX] = mix;
}

static void tack_voice_set_chaos(uint8_t slot, uint8_t x)
{
    /* 6.17 KAOSS-style combo (Teenage Engineering / KAOSS pad energy) — not clean phaser-only.
     * COMP+BASS (0+1): IT_FOLD / IT_FUZZ grit. AIR (2): IT_PHASER sweep.
     * Drive/Tone/Level or Rate/Depth/Fdbk + MIX floor — amount > 0 always hearable. */
    int16_t mix = (int16_t)(58 + ((uint16_t)x * 66u) / 127u);
    if (mix > 122) mix = 122;
    if (slot < 2u) {
        /* Grit layer: FOLD on COMP, FUZZ on BASS — aggressive audible harmonics. */
        int16_t drive = (int16_t)(48 + ((uint16_t)x * 72u) / 127u);
        int16_t tone = (int16_t)(70 - ((uint16_t)x * 28u) / 127u); /* darken as chaos rises */
        int16_t level = (int16_t)(88 + (x >> 3));
        if (drive > 118) drive = 118;
        if (tone < 36) tone = 36;
        if (level > 112) level = 112;
        trk[slot].p[P_ITYPE] = (slot == 0u) ? IT_FOLD : IT_FUZZ;
        trk[slot].p[P_IA] = drive;   /* DRIVE */
        trk[slot].p[P_IB] = tone;    /* TONE */
        trk[slot].p[P_IC] = level;   /* LEVEL */
        trk[slot].p[P_IMIX] = mix;
    } else {
        /* Sweep layer on AIR — phaser even when chaos is the only FX (combo split). */
        int16_t rate = (int16_t)(28 + (x >> 1));
        int16_t depth = (int16_t)(56 + ((uint16_t)x * 68u) / 127u);
        int16_t fbk = (int16_t)(52 + (x >> 1));
        if (depth > 120) depth = 120;
        if (fbk > 118) fbk = 118;
        trk[slot].p[P_ITYPE] = IT_PHASER;
        trk[slot].p[P_IA] = rate;
        trk[slot].p[P_IB] = depth;
        trk[slot].p[P_IC] = fbk;
        trk[slot].p[P_IMIX] = mix;
    }
}

/* Chaos filter layer: best-effort CUT↓ + RES↑ on voices 0..2 (analog P_E4/P_E5).
 * Only while chaos > 0. Chaos→0 clears inserts only — do not fight library restore. */
static void tack_chaos_filter_layer(uint8_t x)
{
    uint32_t i;
    int16_t cut, res;
    if (x == 0u) return;
    cut = (int16_t)(78 - ((uint16_t)x * 50u) / 127u); /* open → tighter as chaos rises */
    res = (int16_t)(36 + ((uint16_t)x * 56u) / 127u); /* RES up */
    if (cut < 18) cut = 18;
    if (res > 110) res = 110;
    for (i = 0; i < 3u; i++) {
        trk[i].p[P_E4] = cut;
        trk[i].p[P_E5] = res;
    }
}

/* Optional light delay color at high chaos — never invents wet when masters are dry. */
static void tack_chaos_color_wash(uint8_t x)
{
    if (x < 90u) return;
    if (song.g[G_DCOLOR] < 58) song.g[G_DCOLOR] = (int16_t)(58 + ((x - 90u) >> 1));
    if (tack_fx_dly > 0u && song.g[G_DTIME] > 3) song.g[G_DTIME] = 2; /* keep short if delay already on */
}

static void tack_voice_clear(uint8_t slot)
{
    trk[slot].p[P_ITYPE] = IT_OFF;
    trk[slot].p[P_IMIX] = 0;
}

static void tack_apply_voice_inserts(void)
{
    uint8_t f = tack_fx_flange > 127u ? 127u : tack_fx_flange;
    uint8_t c = tack_fx_crush > 127u ? 127u : tack_fx_crush;
    uint8_t h = tack_fx_vcho > 127u ? 127u : tack_fx_vcho; /* V CHO */
    uint8_t x = tack_fx_chaos > 127u ? 127u : tack_fx_chaos; /* CHAOS / KAOSS combo */
    uint32_t i;
    uint8_t n_on = (uint8_t)((f > 0u) + (c > 0u) + (h > 0u) + (x > 0u));
    if (n_on == 0u) {
        for (i = 0; i < 3u; i++)
            tack_voice_clear((uint8_t)i);
        /* chaos→0: clear inserts only — leave P_E4/P_E5 for library/style restore */
        tack_apply_tape_delay();
        return;
    }
    if (n_on == 1u) {
        if (x > 0u) {
            /* Chaos alone → intentional combo split (fold/fuzz + phaser), not one type×3. */
            tack_voice_set_chaos(0, x);
            tack_voice_set_chaos(1, x);
            tack_voice_set_chaos(2, x);
            tack_chaos_filter_layer(x);
            tack_chaos_color_wash(x);
        } else {
            /* Single non-chaos insert on all three voices — strongest, always hearable. */
            for (i = 0; i < 3u; i++) {
                if (c > 0u) tack_voice_set_crush((uint8_t)i, c);
                else if (f > 0u) tack_voice_set_flange((uint8_t)i, f);
                else tack_voice_set_chorus((uint8_t)i, h);
            }
        }
        tack_apply_tape_delay();
        return;
    }
    /* Several on — priority crush > chaos > flange > chorus.
     * Winner owns COMP+BASS (0+1); next-highest owns AIR (2). Never trk[3].
     * Chaos on AIR still gets phaser; chaos on 0+1 still gets fold/fuzz. */
    if (c > 0u) {
        tack_voice_set_crush(0, c);
        tack_voice_set_crush(1, c);
        if (x > 0u) tack_voice_set_chaos(2, x);
        else if (f > 0u) tack_voice_set_flange(2, f);
        else if (h > 0u) tack_voice_set_chorus(2, h);
        else tack_voice_clear(2);
    } else if (x > 0u) {
        /* Chaos wants AIR phaser as part of combo — keep AIR for chaos when it wins 0+1. */
        tack_voice_set_chaos(0, x);
        tack_voice_set_chaos(1, x);
        tack_voice_set_chaos(2, x); /* phaser on AIR; flange/chorus yield while chaos is on */
    } else {
        /* flange + chorus, no crush/chaos: flange → COMP+BASS, chorus → AIR */
        tack_voice_set_flange(0, f);
        tack_voice_set_flange(1, f);
        tack_voice_set_chorus(2, h);
    }
    if (x > 0u) {
        tack_chaos_filter_layer(x);
        tack_chaos_color_wash(x);
    }
    tack_apply_tape_delay();
}

/* Map FX-page wet masters onto per-track sends + bus globals.
 * Root cause (3.7): page edited G_DMIX/G_RSIZE/G_CDEPTH only; reverb has no G_RMIX
 * (wet = P_REV send); DELAY MIX defaults were already near max so +K2 was silent. */
static void tack_apply_fx_amounts(void)
{
    /* Weights: comp, bass, air, drums — air carries the room. */
    static const uint8_t W_DLY[4] = {70, 40, 100, 28};
    static const uint8_t W_REV[4] = {85, 48, 100, 58};
    static const uint8_t W_CHO[4] = {40, 12, 100, 0};
    uint32_t i;
    uint8_t d = tack_fx_dly, r = tack_fx_rev, c = tack_fx_cho;
    for (i = 0; i < 4u; i++) {
        trk[i].p[P_DLY]  = (uint8_t)((uint16_t)d * W_DLY[i] / 100u);
        trk[i].p[P_REV]  = (uint8_t)((uint16_t)r * W_REV[i] / 100u);
        trk[i].p[P_CHOR] = (uint8_t)((uint16_t)c * W_CHO[i] / 100u);
    }
    song.g[G_DMIX] = (int16_t)(d == 0 ? 0 : (d < 12 ? 12 : d));
    /* Couple size to amount so REVERB row also moves character; wet is from sends. */
    song.g[G_RSIZE] = (int16_t)(r == 0 ? 36 : (36 + ((uint16_t)r * 90u) / 127u));
    song.g[G_CDEPTH] = (int16_t)c;
    if (d == 0) {
        /* Truly dry: do not revive feedback/color wash when masters are zero. */
        song.g[G_DFDBK] = 0;
    } else if (d >= 32 && song.g[G_DFDBK] < 40) {
        song.g[G_DFDBK] = 40;
    }
    if (d > 0 && song.g[G_DCOLOR] < 40) song.g[G_DCOLOR] = 70;
    tack_apply_voice_inserts(); /* keep FLANGER / BIT REDUC / V CHO / CHAOS / TAPE wired */
}

static void tack_amb_apply_soft_pick(uint8_t tr, uint8_t tex);
/* Ambient soft/zen analog pads (library 1 + Zen). Distinct from Dark Ambient by size/weight. */
static void tack_amb_shape_analog_air(uint8_t bright, uint8_t wave, uint8_t dtn, uint8_t cut, uint8_t res)
{
    /* trk[2] air already set to ANALOG; shape as soft pad/drone. */
    trk[2].p[P_E0] = wave;   /* WAVE: 0 SAW 1 SQR 2 TRI 3 SIN 4 PWM */
    trk[2].p[P_E1] = dtn;
    trk[2].p[P_E3] = bright ? 12 : 6;   /* NOIS soft */
    trk[2].p[P_E4] = cut;               /* CUT LPF */
    trk[2].p[P_E5] = res;
    trk[2].p[P_E6] = 0;                 /* no DRV grit */
    trk[2].p[P_ATK] = bright ? 56 : 100;
    trk[2].p[P_REL] = bright ? 108 : 126;
    trk[2].p[P_CHOR] = bright ? 40 : 28;
    trk[2].p[P_REV]  = bright ? 96 : 118;
    trk[2].p[P_DLY]  = bright ? 48 : 78;
    trk[2].p[P_DIST] = bright ? 6 : 2;
    trk[2].p[P_LRATE] = bright ? 14 : 8; /* slow cutoff drift */
    trk[2].p[P_M1SRC] = 1;               /* LFO */
    trk[2].p[P_M1DST] = 2;               /* CUT */
    trk[2].p[P_M1AMT] = bright ? 18 : 12;
    trk[2].p[P_LEVEL] = bright ? 58 : 46;
    tack_level[2] = trk[2].p[P_LEVEL];
}

/* Dark Ambient — Oberheim-like huge: fat stacked saws, wide detune, slow filter, sombre mass. */
static void tack_amb_shape_ob_huge(uint8_t bright, uint8_t wave, uint8_t dtn, uint8_t cut, uint8_t res)
{
    trk[2].p[P_E0] = wave;
    trk[2].p[P_E1] = dtn;                 /* wide detune = stack feel */
    trk[2].p[P_E2] = 64;                  /* MIX dual-osc center */
    trk[2].p[P_E3] = bright ? 8 : 4;
    trk[2].p[P_E4] = cut;
    trk[2].p[P_E5] = res;
    trk[2].p[P_E6] = bright ? 6 : 2;      /* slight drive for body */
    trk[2].p[P_ATK] = bright ? 70 : 105;
    trk[2].p[P_DEC] = 80;
    trk[2].p[P_SUS] = 110;
    trk[2].p[P_REL] = bright ? 118 : 127;
    trk[2].p[P_CHOR] = bright ? 72 : 58;  /* fat chorus width */
    trk[2].p[P_REV]  = bright ? 100 : 118;
    trk[2].p[P_DLY]  = bright ? 40 : 62;
    trk[2].p[P_DIST] = bright ? 10 : 6;
    trk[2].p[P_LRATE] = bright ? 10 : 5;  /* slow filter */
    trk[2].p[P_M1SRC] = 1;                /* LFO → CUT */
    trk[2].p[P_M1DST] = 2;
    trk[2].p[P_M1AMT] = bright ? 28 : 22;
    trk[2].p[P_LEVEL] = bright ? 78 : 68; /* massive vs Ambient soft */
    tack_level[2] = trk[2].p[P_LEVEL];
}

/* Sci-Fi F/X space — drones / spatial / auto-pan / weird space beds. */
static void tack_amb_shape_fx_space(uint8_t bright, uint8_t wave, uint8_t dtn, uint8_t cut, uint8_t res, uint8_t autopan)
{
    trk[2].p[P_E0] = wave;
    trk[2].p[P_E1] = dtn;
    trk[2].p[P_E2] = 64;
    trk[2].p[P_E3] = bright ? 22 : 14;    /* air / hiss */
    trk[2].p[P_E4] = cut;
    trk[2].p[P_E5] = res;
    trk[2].p[P_E6] = 0;
    trk[2].p[P_ATK] = bright ? 64 : 112;
    trk[2].p[P_REL] = 127;
    trk[2].p[P_CHOR] = bright ? 56 : 36;
    trk[2].p[P_REV]  = bright ? 110 : 124;
    trk[2].p[P_DLY]  = bright ? 70 : 96;
    trk[2].p[P_DIST] = bright ? 8 : 4;
    trk[2].p[P_LRATE] = autopan ? 16 : 7;
    trk[2].p[P_M1SRC] = 1;               /* LFO */
    trk[2].p[P_M1DST] = autopan ? 5 : 2; /* PAN or CUT */
    trk[2].p[P_M1AMT] = autopan ? 48 : 20;
    trk[2].p[P_LEVEL] = bright ? 56 : 44;
    tack_level[2] = trk[2].p[P_LEVEL];
}

static void tack_apply_amb_texture(void)
{
    /* Library textures only — never writes style_idx, scale mode, or kit. */
    uint8_t bank = tack_snd_bank;
    uint8_t tex;
    uint8_t bright;
    if (bank == 1u || bank == 5u)
        tex = tack_amb_tex % TACK_AMB_SOFT_N;
    else
        tex = tack_amb_tex % TACK_AMB_TEX_N;
    bright = (bank == 1u || bank == 5u)
        ? (tex >= (TACK_AMB_SOFT_N - 1u))
        : (tex == 11u);

    if (bank == 1u || bank == 5u) {
        /* AMBIENT / ZEN — soft/zen analog pads (6.13 catalog via soft_pick). */
        uint8_t soft = tack_amb_tex % TACK_AMB_SOFT_N;
        bright = (soft >= (TACK_AMB_SOFT_N - 1u));
        set_engine_of(&trk[0], 0);
        apply_preset_to(&trk[0], 10); /* WIND bed */
        if (trk[0].preset == 10) {
            trk[0].p[P_E3] = 16;
            trk[0].p[P_E4] = 42;
            trk[0].p[P_E5] = 20;
            trk[0].p[P_DIST] = 3;
            trk[0].p[P_ATK] = 90; trk[0].p[P_REL] = 120;
            trk[0].p[P_LEVEL] = bright ? 62 : 50;
            tack_level[0] = trk[0].p[P_LEVEL];
            trk[0].p[P_M1AMT] = 8; trk[0].p[P_LRATE] = 8;
        }
        set_engine_of(&trk[1], 0); apply_preset_to(&trk[1], 7);
        trk[1].p[P_ATK] = 40; trk[1].p[P_REL] = 100;
        trk[1].p[P_E4] = 38; trk[1].p[P_LEVEL] = 92;
        tack_level[1] = trk[1].p[P_LEVEL];
        tack_amb_apply_soft_pick(2, soft);
        if (bank == 5u && !bright) {
            if (trk[2].p[P_LEVEL] > 42) trk[2].p[P_LEVEL] = 42;
            tack_level[2] = trk[2].p[P_LEVEL];
            if (trk[2].p[P_REV] < 110) trk[2].p[P_REV] = 110;
        }
        song.g[G_RDAMP] = bright ? 22 : 8;
        song.g[G_CDEPTH] = bright ? 38 : 28;
        song.g[G_CRATE] = bright ? 12 : 6;
        song.g[G_RSIZE] = bright ? 100 : 120;
        tack_shimmer = bright ? 1 : 0;
        tack_drm_dist = bright ? 8 : 4;
        tack_drm_comp = bright ? 14 : 10;
        tack_pong = 0;
    } else if (bank == 2u) {
        /* DARK AMBIENT — analog HUGE Oberheim-like (not Ambient+darkish). */
        set_engine_of(&trk[0], 0);
        apply_preset_to(&trk[0], bright ? 11 : 1); /* STRINGS / SOFT PAD bed */
        trk[0].p[P_E0] = 0; trk[0].p[P_E1] = bright ? 64 : 78;
        trk[0].p[P_E2] = 64; trk[0].p[P_E4] = bright ? 48 : 36;
        trk[0].p[P_E5] = 24; trk[0].p[P_E6] = 4;
        trk[0].p[P_ATK] = 95; trk[0].p[P_REL] = 124;
        trk[0].p[P_CHOR] = 64; trk[0].p[P_REV] = 110;
        trk[0].p[P_LEVEL] = bright ? 70 : 58;
        tack_level[0] = trk[0].p[P_LEVEL];
        trk[0].p[P_LRATE] = 5; trk[0].p[P_M1SRC] = 1; trk[0].p[P_M1DST] = 2; trk[0].p[P_M1AMT] = 18;

        set_engine_of(&trk[1], 0); apply_preset_to(&trk[1], 7); /* SUB under mass */
        trk[1].p[P_ATK] = 50; trk[1].p[P_REL] = 110;
        trk[1].p[P_E4] = 32; trk[1].p[P_LEVEL] = 100;
        tack_level[1] = trk[1].p[P_LEVEL];

        if (bright) {
            set_engine_of(&trk[2], 0); apply_preset_to(&trk[2], 1);
            tack_amb_shape_ob_huge(1, 0, 88, 68, 26);
        } else {
            switch (tex) {
            case 0: /* Fat Stack */
                set_engine_of(&trk[2], 0); apply_preset_to(&trk[2], 1);
                tack_amb_shape_ob_huge(0, 0, 92, 42, 28); break;
            case 1: /* OB Wide */
                set_engine_of(&trk[2], 0); apply_preset_to(&trk[2], 11);
                tack_amb_shape_ob_huge(0, 0, 100, 38, 22); break;
            case 2: /* Huge Saw */
                set_engine_of(&trk[2], 0); apply_preset_to(&trk[2], 1);
                tack_amb_shape_ob_huge(0, 0, 84, 44, 30);
                trk[2].p[P_LEVEL] = 74; tack_level[2] = 74; break;
            case 3: /* Stack Det */
                set_engine_of(&trk[2], 0); apply_preset_to(&trk[2], 3);
                tack_amb_shape_ob_huge(0, 4, 96, 40, 26); break;
            case 4: /* Poly Mass */
                set_engine_of(&trk[2], 0); apply_preset_to(&trk[2], 1);
                tack_amb_shape_ob_huge(0, 0, 78, 46, 20);
                trk[2].p[P_CHOR] = 80; break;
            case 5: /* Low Mass */
                set_engine_of(&trk[2], 0); apply_preset_to(&trk[2], 7);
                tack_amb_shape_ob_huge(0, 0, 70, 28, 18);
                trk[2].p[P_LEVEL] = 62; tack_level[2] = 62; break;
            case 6: /* Slow Sweep */
                set_engine_of(&trk[2], 0); apply_preset_to(&trk[2], 1);
                tack_amb_shape_ob_huge(0, 0, 86, 34, 36);
                trk[2].p[P_LRATE] = 3; trk[2].p[P_M1AMT] = 36; break;
            case 7: /* Brass Wall */
                set_engine_of(&trk[2], 0); apply_preset_to(&trk[2], 9); /* BRASS */
                tack_amb_shape_ob_huge(0, 0, 74, 48, 24);
                trk[2].p[P_ATK] = 88; break;
            case 8: /* Dark Choir */
                set_engine_of(&trk[2], 0); apply_preset_to(&trk[2], 11);
                tack_amb_shape_ob_huge(0, 0, 90, 36, 22);
                trk[2].p[P_REV] = 124; break;
            case 9: /* Filter Bed */
                set_engine_of(&trk[2], 0); apply_preset_to(&trk[2], 1);
                tack_amb_shape_ob_huge(0, 0, 80, 30, 42);
                trk[2].p[P_LRATE] = 4; trk[2].p[P_M1AMT] = 40; break;
            default: /* 10 Unison Bed */
                set_engine_of(&trk[2], 0); apply_preset_to(&trk[2], 1);
                tack_amb_shape_ob_huge(0, 0, 108, 40, 26);
                trk[2].p[P_CHOR] = 88; break;
            }
        }
        song.g[G_RDAMP] = bright ? 14 : 4;
        song.g[G_CDEPTH] = bright ? 72 : 58; /* weighty chorus */
        song.g[G_CRATE] = bright ? 10 : 4;
        song.g[G_RSIZE] = bright ? 112 : 124;
        tack_shimmer = 0;
        tack_drm_dist = bright ? 12 : 8;
        tack_drm_comp = bright ? 20 : 16;
        tack_pong = 0;
        if (trk[3].p[P_LEVEL] < 56) trk[3].p[P_LEVEL] = 60;
        tack_level[3] = trk[3].p[P_LEVEL];
    } else if (bank == 3u) {
        /* SCI-FI — F/X space: drones, spatial, auto-pan, weird — not DX7/bell stubs. */
        set_engine_of(&trk[1], 0); apply_preset_to(&trk[1], 7);
        trk[1].p[P_ATK] = 60; trk[1].p[P_REL] = 118;
        trk[1].p[P_E4] = 30; trk[1].p[P_LEVEL] = 86;
        tack_level[1] = trk[1].p[P_LEVEL];

        if (bright) {
            set_engine_of(&trk[0], 0); apply_preset_to(&trk[0], 1);
            set_engine_of(&trk[2], 0); apply_preset_to(&trk[2], 1);
            tack_amb_shape_fx_space(1, 0, 28, 70, 18, 1);
            tack_pong = 1;
        } else {
            switch (tex) {
            case 0: /* Space Drone */
                set_engine_of(&trk[0], 0); apply_preset_to(&trk[0], 7);
                set_engine_of(&trk[2], 0); apply_preset_to(&trk[2], 7);
                tack_amb_shape_fx_space(0, 3, 10, 32, 12, 0);
                trk[2].p[P_LEVEL] = 40; tack_level[2] = 40;
                tack_pong = 0; break;
            case 1: /* Auto Pan */
                set_engine_of(&trk[0], 0); apply_preset_to(&trk[0], 1);
                set_engine_of(&trk[2], 0); apply_preset_to(&trk[2], 1);
                tack_amb_shape_fx_space(0, 0, 24, 50, 16, 1);
                tack_pong = 1; break;
            case 2: /* Far Field */
                set_engine_of(&trk[0], 0); apply_preset_to(&trk[0], 10);
                set_engine_of(&trk[2], 0); apply_preset_to(&trk[2], 10);
                tack_amb_shape_fx_space(0, 0, 18, 40, 14, 0);
                trk[2].p[P_REV] = 126; trk[2].p[P_DLY] = 110;
                tack_pong = 1; break;
            case 3: /* Weird Orbit */
                set_engine_of(&trk[0], 2); apply_preset_to(&trk[0], 2);
                set_engine_of(&trk[2], 2); apply_preset_to(&trk[2], 5); /* WIRE — never BELL(4) */
                trk[2].p[P_ATK] = 100; trk[2].p[P_REL] = 126;
                trk[2].p[P_CHOR] = 48; trk[2].p[P_REV] = 120; trk[2].p[P_DLY] = 88;
                trk[2].p[P_LRATE] = 14; trk[2].p[P_M1SRC] = 1; trk[2].p[P_M1DST] = 5; trk[2].p[P_M1AMT] = 44;
                trk[2].p[P_LEVEL] = 46; tack_level[2] = 46;
                tack_pong = 1; break;
            case 4: /* Void Bed */
                set_engine_of(&trk[0], 0); apply_preset_to(&trk[0], 7);
                set_engine_of(&trk[2], 0); apply_preset_to(&trk[2], 1);
                tack_amb_shape_fx_space(0, 3, 6, 26, 8, 0);
                trk[2].p[P_LEVEL] = 36; tack_level[2] = 36;
                tack_pong = 0; break;
            case 5: /* Stereo Sweep */
                set_engine_of(&trk[0], 0); apply_preset_to(&trk[0], 1);
                set_engine_of(&trk[2], 0); apply_preset_to(&trk[2], 3);
                tack_amb_shape_fx_space(0, 4, 30, 48, 22, 1);
                trk[2].p[P_LRATE] = 11; tack_pong = 1; break;
            case 6: /* Long Trail */
                set_engine_of(&trk[0], 0); apply_preset_to(&trk[0], 1);
                set_engine_of(&trk[2], 0); apply_preset_to(&trk[2], 1);
                tack_amb_shape_fx_space(0, 0, 16, 44, 12, 0);
                trk[2].p[P_ATK] = 118; trk[2].p[P_DLY] = 118; trk[2].p[P_REV] = 126;
                tack_pong = 1; break;
            case 7: /* Radio Hiss — NOISE engine */
                set_engine_of(&trk[0], 11); apply_preset_to(&trk[0], 0); /* WIND */
                set_engine_of(&trk[2], 11); apply_preset_to(&trk[2], 1); /* RAIN */
                trk[2].p[P_ATK] = 90; trk[2].p[P_REL] = 124;
                trk[2].p[P_REV] = 118; trk[2].p[P_DLY] = 80;
                trk[2].p[P_LRATE] = 12; trk[2].p[P_M1SRC] = 1; trk[2].p[P_M1DST] = 5; trk[2].p[P_M1AMT] = 40;
                trk[2].p[P_LEVEL] = 42; tack_level[2] = 42;
                tack_pong = 1; break;
            case 8: /* Grav Well */
                set_engine_of(&trk[0], 0); apply_preset_to(&trk[0], 7);
                set_engine_of(&trk[2], 0); apply_preset_to(&trk[2], 7);
                tack_amb_shape_fx_space(0, 0, 40, 22, 30, 0);
                trk[2].p[P_LRATE] = 4; trk[2].p[P_M1AMT] = 32;
                tack_pong = 0; break;
            case 9: /* Spin Pad */
                set_engine_of(&trk[0], 0); apply_preset_to(&trk[0], 1);
                set_engine_of(&trk[2], 0); apply_preset_to(&trk[2], 1);
                tack_amb_shape_fx_space(0, 0, 34, 52, 18, 1);
                trk[2].p[P_LRATE] = 20; trk[2].p[P_M1AMT] = 56;
                tack_pong = 1; break;
            default: /* 10 Deep Space */
                set_engine_of(&trk[0], 0); apply_preset_to(&trk[0], 10);
                set_engine_of(&trk[2], 0); apply_preset_to(&trk[2], 1);
                tack_amb_shape_fx_space(0, 2, 12, 36, 10, 0);
                trk[2].p[P_REV] = 126; trk[2].p[P_DLY] = 108;
                tack_pong = 1; break;
            }
        }
        song.g[G_CDEPTH] = bright ? 58 : 40;
        song.g[G_CRATE] = bright ? 22 : 8;
        song.g[G_RDAMP] = bright ? 24 : 10;
        song.g[G_RSIZE] = bright ? 108 : 124;
        song.g[G_DMIX] = 96;
        if (tack_pong) tack_apply_pong();
        tack_shimmer = bright ? 2 : 1;
        tack_drm_dist = 10; tack_drm_comp = 16;
        tack_level[3] = trk[3].p[P_LEVEL] = 68;
    } else
        return;
    /* Kit independence: textures may touch drum level but never replace a held kit. */
    if (tack_kit_sel <= 3u) {
        apply_preset_to(&trk[3], TACK_KIT_PRESET[tack_kit_sel]);
        tack_beat_kit = (tack_kit_sel >= 2u) ? (uint8_t)(tack_kit_sel - 1u) : 0u;
    }
    tack_fx_rev = (uint8_t)trk[2].p[P_REV];
    tack_fx_cho = (uint8_t)trk[2].p[P_CHOR];
    if ((uint8_t)trk[2].p[P_DLY] > tack_fx_dly) tack_fx_dly = (uint8_t)trk[2].p[P_DLY];
    tack_apply_fx_amounts();
    tack_apply_shimmer();
    tack_apply_drum_bus();
    {
        uint32_t i;
        for (i = 0; i < 4u; i++)
            if (!(tack_mute & (1u << i)))
                tack_level[i] = trk[i].p[P_LEVEL];
    }
    tack_assert_poly_chords();
}

static void tack_apply_pong(void)
{
    /* No true stereo ping-pong bus — approximate with brighter feedback color + higher mix. */
    if (tack_pong) {
        song.g[G_DCOLOR] = 110;
        if (song.g[G_DMIX] < 100) song.g[G_DMIX] = 100;
        if (song.g[G_DFDBK] < 80) song.g[G_DFDBK] = 80;
    } else {
        song.g[G_DCOLOR] = 70;
    }
}


/* ---- 6.1 library sound catalog (EDIT two-step + LFO per-track) ---- */
static uint8_t tack_lib_sound_n(uint8_t bank)
{
    switch (bank % TACK_LIB_N) {
    case 0: return 4u;                 /* KITS: 808 909 CR78 707 */
    case 1: return TACK_AMB_SOFT_N;    /* AMBIENT soft analog/zen (6.13 ×2) */
    case 2: return TACK_AMB_TEX_N;     /* DARK AMB Oberheim-huge */
    case 3: return TACK_AMB_TEX_N;     /* SCI-FI F/X space */
    case 4: return 6u;                 /* 303/ACID */
    case 5: return 8u;                 /* ZEN calm subset */
    case 6: return TACK_TECHNO_N;      /* TECHNO club */
    case 7: return TACK_CLASSIC_N;     /* CLASSIC DX7 */
    case 8: return TACK_IND_N;         /* INDUSTRIAL foundry */
    case 9: return TACK_JUNO_N;        /* JUNO poly + chorus */
    default: return TACK_AMB_SOFT_N;
    }
}

static const char *tack_lib_sound_name(uint8_t bank, uint8_t idx)
{
    bank = (uint8_t)(bank % TACK_LIB_N);
    idx = (uint8_t)(idx % tack_lib_sound_n(bank));
    if (bank == 0u) return TACK_KIT_TAG[idx];
    if (bank == 4u) return TACK_ACID_NAME[idx];
    if (bank == 5u) return TACK_AMB_TEX_NAME[TACK_ZEN_TEX[idx] % TACK_AMB_SOFT_N];
    if (bank == 6u) return TACK_TECHNO_NAME[idx];
    if (bank == 7u) return TACK_CLASSIC_NAME[idx];
    if (bank == 8u) return TACK_IND_NAME[idx];
    if (bank == 9u) return TACK_JUNO_NAME[idx];
    if (bank == 2u) return TACK_DARK_TEX_NAME[idx % TACK_AMB_TEX_N];
    if (bank == 3u) return TACK_SCIFI_TEX_NAME[idx % TACK_AMB_TEX_N];
    return TACK_AMB_TEX_NAME[idx % TACK_AMB_SOFT_N]; /* Ambient soft */
}


/* 6.4: label for a track = catalog name at that track's library pick (not engine factory). */
static const char *tack_trk_lib_name(uint8_t tr)
{
    tr &= 3u;
    if (tr == 3u)
        return tack_kit_label(&trk[3]);
    return tack_lib_sound_name(tack_snd_bank, tack_trk_lib_pick[tr]);
}

static void tack_trk_lib_set(uint8_t tr, uint8_t pick)
{
    tr &= 3u;
    pick = (uint8_t)(pick % tack_lib_sound_n(tack_snd_bank));
    tack_trk_lib_pick[tr] = pick;
    if (tr == (tack_mute_sel & 3u))
        tack_snd_pick = pick;
}

static void tack_trk_lib_set_all_voices(uint8_t pick)
{
    uint8_t i;
    pick = (uint8_t)(pick % tack_lib_sound_n(tack_snd_bank));
    for (i = 0; i < 3u; i++)
        tack_trk_lib_pick[i] = pick;
    tack_snd_pick = pick;
}
static uint8_t tack_lib_tex_from_pick(uint8_t bank, uint8_t pick)
{
    bank = (uint8_t)(bank % TACK_LIB_N);
    pick = (uint8_t)(pick % tack_lib_sound_n(bank));
    if (bank == 5u) return TACK_ZEN_TEX[pick];
    if (bank >= 1u && bank <= 3u) return pick;
    return 0;
}

/* Shape one track as a soft analog/wave bed (no BELL). Used by LFO per-track assign. */
static void tack_shape_track_analog_air(uint8_t tr, uint8_t bright, uint8_t wave, uint8_t dtn, uint8_t cut, uint8_t res)
{
    track_t *t = &trk[tr & 3u];
    t->p[P_E0] = wave;
    t->p[P_E1] = dtn;
    t->p[P_E3] = bright ? 12 : 6;
    t->p[P_E4] = cut;
    t->p[P_E5] = res;
    t->p[P_E6] = 0;
    t->p[P_ATK] = bright ? 56 : 100;
    t->p[P_REL] = bright ? 108 : 126;
    t->p[P_CHOR] = bright ? 40 : 28;
    t->p[P_REV]  = bright ? 96 : 118;
    t->p[P_DLY]  = bright ? 48 : 78;
    t->p[P_DIST] = bright ? 6 : 2;
    t->p[P_LRATE] = bright ? 14 : 8;
    t->p[P_M1SRC] = 1; t->p[P_M1DST] = 2;
    t->p[P_M1AMT] = bright ? 18 : 12;
    t->p[P_LEVEL] = bright ? 58 : 46;
    tack_level[tr & 3u] = t->p[P_LEVEL];
}


/* 6.13: Ambient soft catalog → one lane. Soft analog pads/harmonics/drones; no DX7/bells.
 * bright = last slot (BRIGHT~). Dark Ambient stays on its own Oberheim path. */
static void tack_amb_apply_soft_pick(uint8_t tr, uint8_t tex)
{
    uint8_t bright = (tex >= (TACK_AMB_SOFT_N - 1u));
    tr &= 3u;
    if (tr == 3u) return;
    if (bright) {
        set_engine_of(&trk[tr], 0); apply_preset_to(&trk[tr], 1);
        tack_shape_track_analog_air(tr, 1, 0, 22, 72, 22);
        return;
    }
    switch (tex % TACK_AMB_SOFT_N) {
    case 0: set_engine_of(&trk[tr], 0); apply_preset_to(&trk[tr], 1);
            tack_shape_track_analog_air(tr, 0, 2, 10, 48, 14); break;
    case 1: set_engine_of(&trk[tr], 0); apply_preset_to(&trk[tr], 1);
            tack_shape_track_analog_air(tr, 0, 0, 16, 52, 16); break;
    case 2: set_engine_of(&trk[tr], 0); apply_preset_to(&trk[tr], 3);
            tack_shape_track_analog_air(tr, 0, 4, 20, 50, 18); break;
    case 3: set_engine_of(&trk[tr], 0); apply_preset_to(&trk[tr], 1);
            tack_shape_track_analog_air(tr, 0, 0, 36, 56, 14);
            trk[tr].p[P_LRATE] = 6; trk[tr].p[P_M1AMT] = 22; break;
    case 4: set_engine_of(&trk[tr], 2); apply_preset_to(&trk[tr], 2); /* harmonics bed */
            trk[tr].p[P_ATK] = 96; trk[tr].p[P_REL] = 124;
            trk[tr].p[P_CHOR] = 32; trk[tr].p[P_REV] = 116;
            trk[tr].p[P_DLY] = 70; trk[tr].p[P_LEVEL] = 48;
            tack_level[tr] = 48; break;
    case 5: set_engine_of(&trk[tr], 0); apply_preset_to(&trk[tr], 7);
            tack_shape_track_analog_air(tr, 0, 3, 8, 34, 10);
            trk[tr].p[P_LEVEL] = 40; tack_level[tr] = 40; break;
    case 6: set_engine_of(&trk[tr], 0); apply_preset_to(&trk[tr], 10);
            tack_shape_track_analog_air(tr, 0, 0, 8, 44, 22);
            trk[tr].p[P_E3] = 28; break;
    case 7: set_engine_of(&trk[tr], 0); apply_preset_to(&trk[tr], 1);
            tack_shape_track_analog_air(tr, 0, 0, 14, 50, 12);
            trk[tr].p[P_ATK] = 110; trk[tr].p[P_REL] = 127; break;
    case 8: set_engine_of(&trk[tr], 0); apply_preset_to(&trk[tr], 11);
            tack_shape_track_analog_air(tr, 0, 0, 18, 40, 28);
            trk[tr].p[P_REV] = 122; trk[tr].p[P_CHOR] = 22; break;
    case 9: set_engine_of(&trk[tr], 0); apply_preset_to(&trk[tr], 3);
            tack_shape_track_analog_air(tr, 0, 4, 24, 46, 20);
            trk[tr].p[P_LRATE] = 5; trk[tr].p[P_M1AMT] = 28; break;
    case 10: /* Harm Pad — soft analog, not FM bell */
            set_engine_of(&trk[tr], 0); apply_preset_to(&trk[tr], 1);
            tack_shape_track_analog_air(tr, 0, 0, 20, 54, 16);
            trk[tr].p[P_CHOR] = 36; trk[tr].p[P_REV] = 112; break;
    case 11: /* Soft PWM */
            set_engine_of(&trk[tr], 0); apply_preset_to(&trk[tr], 3);
            tack_shape_track_analog_air(tr, 0, 4, 18, 46, 22);
            trk[tr].p[P_LRATE] = 7; trk[tr].p[P_M1AMT] = 24; break;
    case 12: /* Tape Wash */
            set_engine_of(&trk[tr], 0); apply_preset_to(&trk[tr], 10);
            tack_shape_track_analog_air(tr, 0, 0, 12, 42, 14);
            trk[tr].p[P_DLY] = 96; trk[tr].p[P_REV] = 118; trk[tr].p[P_ATK] = 100; break;
    case 13: /* Harm Fog */
            set_engine_of(&trk[tr], 2); apply_preset_to(&trk[tr], 2);
            trk[tr].p[P_ATK] = 108; trk[tr].p[P_REL] = 126;
            trk[tr].p[P_CHOR] = 28; trk[tr].p[P_REV] = 120;
            trk[tr].p[P_DLY] = 84; trk[tr].p[P_LEVEL] = 44; tack_level[tr] = 44; break;
    case 14: /* Velvet Tri */
            set_engine_of(&trk[tr], 0); apply_preset_to(&trk[tr], 1);
            tack_shape_track_analog_air(tr, 0, 2, 8, 44, 12);
            trk[tr].p[P_LEVEL] = 44; tack_level[tr] = 44; break;
    case 15: /* Sub Bloom */
            set_engine_of(&trk[tr], 0); apply_preset_to(&trk[tr], 7);
            tack_shape_track_analog_air(tr, 0, 3, 6, 28, 8);
            trk[tr].p[P_LEVEL] = 38; tack_level[tr] = 38; trk[tr].p[P_ATK] = 118; break;
    case 16: /* Slow Choir — analog STRINGS pad, not DX7 */
            set_engine_of(&trk[tr], 0); apply_preset_to(&trk[tr], 11);
            tack_shape_track_analog_air(tr, 0, 0, 22, 48, 18);
            trk[tr].p[P_CHOR] = 48; trk[tr].p[P_REV] = 120; trk[tr].p[P_ATK] = 112; break;
    case 17: /* Dust Pad */
            set_engine_of(&trk[tr], 0); apply_preset_to(&trk[tr], 10);
            tack_shape_track_analog_air(tr, 0, 0, 10, 40, 16);
            trk[tr].p[P_E3] = 22; trk[tr].p[P_LEVEL] = 42; tack_level[tr] = 42; break;
    case 18: /* Amber Drone */
            set_engine_of(&trk[tr], 0); apply_preset_to(&trk[tr], 7);
            tack_shape_track_analog_air(tr, 0, 0, 14, 36, 20);
            trk[tr].p[P_ATK] = 120; trk[tr].p[P_REL] = 127;
            trk[tr].p[P_LEVEL] = 40; tack_level[tr] = 40; break;
    case 19: /* Soft Fifths — gentle dual feel via detune */
            set_engine_of(&trk[tr], 0); apply_preset_to(&trk[tr], 1);
            tack_shape_track_analog_air(tr, 0, 0, 40, 50, 14);
            trk[tr].p[P_CHOR] = 40; break;
    case 20: /* Quiet Saw */
            set_engine_of(&trk[tr], 0); apply_preset_to(&trk[tr], 1);
            tack_shape_track_analog_air(tr, 0, 0, 12, 46, 10);
            trk[tr].p[P_LEVEL] = 42; tack_level[tr] = 42; break;
    case 21: /* Cloud Bed */
            set_engine_of(&trk[tr], 0); apply_preset_to(&trk[tr], 10);
            tack_shape_track_analog_air(tr, 0, 2, 16, 52, 12);
            trk[tr].p[P_REV] = 124; trk[tr].p[P_DLY] = 88; break;
    case 22: /* Deep Wash */
            set_engine_of(&trk[tr], 0); apply_preset_to(&trk[tr], 11);
            tack_shape_track_analog_air(tr, 0, 0, 10, 34, 24);
            trk[tr].p[P_ATK] = 116; trk[tr].p[P_REV] = 126;
            trk[tr].p[P_LEVEL] = 40; tack_level[tr] = 40; break;
    default:
            set_engine_of(&trk[tr], 0); apply_preset_to(&trk[tr], 1);
            tack_shape_track_analog_air(tr, 0, 0, 16, 48, 14); break;
    }
}


static void tack_shape_track_ob_huge(uint8_t tr, uint8_t bright, uint8_t wave, uint8_t dtn, uint8_t cut, uint8_t res)
{
    track_t *t = &trk[tr & 3u];
    t->p[P_E0] = wave; t->p[P_E1] = dtn; t->p[P_E2] = 64;
    t->p[P_E3] = bright ? 8 : 4; t->p[P_E4] = cut; t->p[P_E5] = res;
    t->p[P_E6] = bright ? 6 : 2;
    t->p[P_ATK] = bright ? 70 : 105; t->p[P_DEC] = 80; t->p[P_SUS] = 110;
    t->p[P_REL] = bright ? 118 : 127;
    t->p[P_CHOR] = bright ? 72 : 58; t->p[P_REV] = bright ? 100 : 118;
    t->p[P_DLY] = bright ? 40 : 62; t->p[P_DIST] = bright ? 10 : 6;
    t->p[P_LRATE] = bright ? 10 : 5;
    t->p[P_M1SRC] = 1; t->p[P_M1DST] = 2; t->p[P_M1AMT] = bright ? 28 : 22;
    t->p[P_LEVEL] = bright ? 78 : 68;
    tack_level[tr & 3u] = t->p[P_LEVEL];
}

static void tack_shape_track_fx_space(uint8_t tr, uint8_t bright, uint8_t wave, uint8_t dtn, uint8_t cut, uint8_t res, uint8_t autopan)
{
    track_t *t = &trk[tr & 3u];
    t->p[P_E0] = wave; t->p[P_E1] = dtn; t->p[P_E2] = 64;
    t->p[P_E3] = bright ? 22 : 14; t->p[P_E4] = cut; t->p[P_E5] = res; t->p[P_E6] = 0;
    t->p[P_ATK] = bright ? 64 : 112; t->p[P_REL] = 127;
    t->p[P_CHOR] = bright ? 56 : 36; t->p[P_REV] = bright ? 110 : 124;
    t->p[P_DLY] = bright ? 70 : 96; t->p[P_DIST] = bright ? 8 : 4;
    t->p[P_LRATE] = autopan ? 16 : 7;
    t->p[P_M1SRC] = 1; t->p[P_M1DST] = autopan ? 5 : 2; t->p[P_M1AMT] = autopan ? 48 : 20;
    t->p[P_LEVEL] = bright ? 56 : 44;
    tack_level[tr & 3u] = t->p[P_LEVEL];
}

/* ---- 6.5 TECHNO / CLASSIC per-track shapers (library presets only) ---- */
static void tack_shape_track_techno(uint8_t tr, uint8_t pick)
{
    track_t *t = &trk[tr & 3u];
    pick %= TACK_TECHNO_N;
    switch (pick) {
    case 0: /* Hard Saw */
        set_engine_of(t, 0); apply_preset_to(t, 0); /* SAW LEAD */
        t->p[P_E0] = 0; t->p[P_E1] = 18; t->p[P_E4] = 96; t->p[P_E5] = 55;
        t->p[P_E6] = 28; t->p[P_ATK] = 0; t->p[P_DEC] = 50; t->p[P_SUS] = 70;
        t->p[P_REL] = 40; t->p[P_DIST] = 22; t->p[P_CHOR] = 8; t->p[P_REV] = 18;
        t->p[P_DLY] = 12; t->p[P_LEVEL] = 100; break;
    case 1: /* Club Stab */
        set_engine_of(t, 0); apply_preset_to(t, 8); /* PLUCK */
        t->p[P_E0] = 0; t->p[P_E1] = 10; t->p[P_E4] = 88; t->p[P_E5] = 48;
        t->p[P_ATK] = 0; t->p[P_DEC] = 28; t->p[P_SUS] = 10; t->p[P_REL] = 22;
        t->p[P_DIST] = 18; t->p[P_CHOR] = 4; t->p[P_REV] = 12; t->p[P_DLY] = 8;
        t->p[P_LEVEL] = 108; break;
    case 2: /* Acid Hit — short acid-adjacent stab ≠ 303 bassline pack */
        set_engine_of(t, 0); apply_preset_to(t, 4); /* ACID */
        t->p[P_E4] = 72; t->p[P_E5] = 110; t->p[P_E6] = 36;
        t->p[P_ATK] = 0; t->p[P_DEC] = 22; t->p[P_SUS] = 8; t->p[P_REL] = 18;
        t->p[P_DIST] = 30; t->p[P_CHOR] = 0; t->p[P_REV] = 8; t->p[P_DLY] = 6;
        t->p[P_LEVEL] = 112; break;
    case 3: /* Indust — industrial noise as tone */
        set_engine_of(t, 11); apply_preset_to(t, 3); /* METAL */
        t->p[P_ATK] = 0; t->p[P_DEC] = 35; t->p[P_SUS] = 20; t->p[P_REL] = 40;
        t->p[P_DIST] = 24; t->p[P_CHOR] = 6; t->p[P_REV] = 28; t->p[P_DLY] = 16;
        t->p[P_LEVEL] = 88; break;
    case 4: /* Cold Pad */
        set_engine_of(t, 0); apply_preset_to(t, 1); /* SOFT PAD */
        t->p[P_E0] = 0; t->p[P_E1] = 28; t->p[P_E4] = 48; t->p[P_E5] = 18;
        t->p[P_ATK] = 70; t->p[P_DEC] = 80; t->p[P_SUS] = 100; t->p[P_REL] = 110;
        t->p[P_DIST] = 4; t->p[P_CHOR] = 36; t->p[P_REV] = 70; t->p[P_DLY] = 40;
        t->p[P_LEVEL] = 62; break;
    case 5: /* Offbeat chord stab */
        set_engine_of(t, 0); apply_preset_to(t, 0);
        t->p[P_E0] = 0; t->p[P_E1] = 14; t->p[P_E4] = 80; t->p[P_E5] = 40;
        t->p[P_ATK] = 0; t->p[P_DEC] = 32; t->p[P_SUS] = 18; t->p[P_REL] = 28;
        t->p[P_DIST] = 14; t->p[P_CHOR] = 12; t->p[P_REV] = 20; t->p[P_DLY] = 10;
        t->p[P_LEVEL] = 96; break;
    case 6: /* Pulse Saw */
        set_engine_of(t, 0); apply_preset_to(t, 0);
        t->p[P_E0] = 4; t->p[P_E1] = 22; t->p[P_E4] = 90; t->p[P_E5] = 42; /* PWM-ish */
        t->p[P_ATK] = 4; t->p[P_DEC] = 60; t->p[P_SUS] = 80; t->p[P_REL] = 50;
        t->p[P_DIST] = 16; t->p[P_LRATE] = 18; t->p[P_M1SRC] = 1; t->p[P_M1DST] = 2;
        t->p[P_M1AMT] = 40; t->p[P_CHOR] = 10; t->p[P_REV] = 16; t->p[P_LEVEL] = 98;
        break;
    case 7: /* Warehouse */
        set_engine_of(t, 0); apply_preset_to(t, 2); /* SQR BASS */
        t->p[P_E0] = 1; t->p[P_E4] = 36; t->p[P_E5] = 70; t->p[P_E6] = 40;
        t->p[P_ATK] = 0; t->p[P_DEC] = 55; t->p[P_SUS] = 40; t->p[P_REL] = 45;
        t->p[P_DIST] = 28; t->p[P_CHOR] = 0; t->p[P_REV] = 14; t->p[P_DLY] = 8;
        t->p[P_LEVEL] = 118; break;
    case 8: /* Rave Stab */
        set_engine_of(t, 0); apply_preset_to(t, 6); /* RAVE */
        t->p[P_E0] = 4; t->p[P_E1] = 34; t->p[P_E4] = 92; t->p[P_E5] = 50;
        t->p[P_ATK] = 0; t->p[P_DEC] = 26; t->p[P_SUS] = 12; t->p[P_REL] = 24;
        t->p[P_DIST] = 26; t->p[P_CHOR] = 20; t->p[P_REV] = 22; t->p[P_LEVEL] = 104;
        break;
    case 9: /* Noise Tone */
        set_engine_of(t, 11); apply_preset_to(t, 2); /* ARCADE */
        t->p[P_ATK] = 0; t->p[P_DEC] = 40; t->p[P_SUS] = 30; t->p[P_REL] = 35;
        t->p[P_DIST] = 20; t->p[P_CHOR] = 4; t->p[P_REV] = 18; t->p[P_LEVEL] = 80;
        break;
    case 10: /* Tech Brass */
        set_engine_of(t, 0); apply_preset_to(t, 9); /* BRASS */
        t->p[P_E0] = 0; t->p[P_E1] = 12; t->p[P_E4] = 78; t->p[P_E5] = 36;
        t->p[P_ATK] = 8; t->p[P_DEC] = 45; t->p[P_SUS] = 55; t->p[P_REL] = 40;
        t->p[P_DIST] = 12; t->p[P_CHOR] = 16; t->p[P_REV] = 24; t->p[P_LEVEL] = 94;
        break;
    default: /* 11 CLUB LIFT~ */
        set_engine_of(t, 0); apply_preset_to(t, 0);
        t->p[P_E0] = 0; t->p[P_E1] = 40; t->p[P_E4] = 100; t->p[P_E5] = 30;
        t->p[P_ATK] = 20; t->p[P_DEC] = 70; t->p[P_SUS] = 90; t->p[P_REL] = 80;
        t->p[P_DIST] = 10; t->p[P_CHOR] = 28; t->p[P_REV] = 40; t->p[P_DLY] = 28;
        t->p[P_LEVEL] = 90; break;
    }
    tack_level[tr & 3u] = t->p[P_LEVEL];
}

static void tack_shape_track_classic(uint8_t tr, uint8_t pick)
{
    track_t *t = &trk[tr & 3u];
    uint8_t pre;
    pick %= TACK_CLASSIC_N;
    pre = TACK_CLASSIC_PRE[pick];
    set_engine_of(t, ENGI_FM6);
    apply_preset_to(t, pre);
    switch (pick) {
    case 0: /* Tine EP */
        t->p[P_ATK] = 2; t->p[P_DEC] = 70; t->p[P_SUS] = 40; t->p[P_REL] = 55;
        t->p[P_CHOR] = 18; t->p[P_REV] = 28; t->p[P_DLY] = 12; t->p[P_LEVEL] = 96;
        break;
    case 1: /* DX Bell */
        t->p[P_ATK] = 0; t->p[P_DEC] = 90; t->p[P_SUS] = 10; t->p[P_REL] = 100;
        t->p[P_CHOR] = 8; t->p[P_REV] = 55; t->p[P_DLY] = 18; t->p[P_LEVEL] = 88;
        break;
    case 2: /* FM Brass */
        t->p[P_ATK] = 18; t->p[P_DEC] = 50; t->p[P_SUS] = 80; t->p[P_REL] = 45;
        t->p[P_CHOR] = 22; t->p[P_REV] = 30; t->p[P_DLY] = 14; t->p[P_LEVEL] = 100;
        break;
    case 3: /* FM Bass */
        t->p[P_ATK] = 0; t->p[P_DEC] = 45; t->p[P_SUS] = 60; t->p[P_REL] = 35;
        t->p[P_CHOR] = 0; t->p[P_REV] = 8; t->p[P_DLY] = 4; t->p[P_LEVEL] = 118;
        break;
    case 4: /* DX Flute — soft airy FM (PAD shaped) */
        t->p[P_ATK] = 40; t->p[P_DEC] = 60; t->p[P_SUS] = 90; t->p[P_REL] = 70;
        t->p[P_CHOR] = 28; t->p[P_REV] = 40; t->p[P_DLY] = 20; t->p[P_LEVEL] = 78;
        t->p[P_E6] = 40; /* breath */
        break;
    case 5: /* Marimba */
        t->p[P_ATK] = 0; t->p[P_DEC] = 55; t->p[P_SUS] = 8; t->p[P_REL] = 45;
        t->p[P_CHOR] = 4; t->p[P_REV] = 35; t->p[P_DLY] = 16; t->p[P_LEVEL] = 92;
        break;
    case 6: /* DX Organ */
        t->p[P_ATK] = 0; t->p[P_DEC] = 20; t->p[P_SUS] = 120; t->p[P_REL] = 30;
        t->p[P_CHOR] = 30; t->p[P_REV] = 22; t->p[P_DLY] = 8; t->p[P_LEVEL] = 94;
        break;
    case 7: /* DX Pluck */
        t->p[P_ATK] = 0; t->p[P_DEC] = 40; t->p[P_SUS] = 15; t->p[P_REL] = 35;
        t->p[P_CHOR] = 10; t->p[P_REV] = 28; t->p[P_DLY] = 22; t->p[P_LEVEL] = 98;
        break;
    case 8: /* Soft EP */
        t->p[P_ATK] = 6; t->p[P_DEC] = 80; t->p[P_SUS] = 55; t->p[P_REL] = 70;
        t->p[P_CHOR] = 24; t->p[P_REV] = 36; t->p[P_DLY] = 18; t->p[P_LEVEL] = 90;
        break;
    case 9: /* Brass Stab */
        t->p[P_ATK] = 4; t->p[P_DEC] = 28; t->p[P_SUS] = 20; t->p[P_REL] = 22;
        t->p[P_CHOR] = 12; t->p[P_REV] = 18; t->p[P_DLY] = 10; t->p[P_LEVEL] = 104;
        break;
    case 10: /* Choir Pad */
        t->p[P_ATK] = 80; t->p[P_DEC] = 90; t->p[P_SUS] = 110; t->p[P_REL] = 118;
        t->p[P_CHOR] = 48; t->p[P_REV] = 70; t->p[P_DLY] = 36; t->p[P_LEVEL] = 72;
        break;
    default: /* 11 DX LIFT~ */
        t->p[P_ATK] = 30; t->p[P_DEC] = 70; t->p[P_SUS] = 100; t->p[P_REL] = 90;
        t->p[P_CHOR] = 40; t->p[P_REV] = 55; t->p[P_DLY] = 30; t->p[P_LEVEL] = 82;
        break;
    }
    tack_level[tr & 3u] = t->p[P_LEVEL];
}

static void tack_apply_techno_bank(uint8_t pick)
{
    /* Club techno band — ANALOG/NOISE voices; style/kit untouched. */
    pick %= TACK_TECHNO_N;
    tack_shape_track_techno(0, pick == 11u ? 4u : (pick == 3u || pick == 9u ? 0u : pick));
    tack_shape_track_techno(1, 7); /* Warehouse bass under */
    if (pick == 3u || pick == 9u)
        tack_shape_track_techno(2, pick);
    else if (pick == 4u || pick == 11u)
        tack_shape_track_techno(2, pick);
    else
        tack_shape_track_techno(2, pick);
    song.g[G_RDAMP] = 28;
    song.g[G_CDEPTH] = 18;
    song.g[G_CRATE] = 10;
    song.g[G_RSIZE] = 70;
    tack_shimmer = 0;
    tack_drm_dist = 22;
    tack_drm_comp = 30;
    tack_pong = 0;
    tack_mute &= (uint8_t)~((1u << 0) | (1u << 1) | (1u << 2));
}

static void tack_apply_classic_bank(uint8_t pick)
{
    /* Classic DX7 band — FM6 voices; style/kit untouched. */
    pick %= TACK_CLASSIC_N;
    tack_shape_track_classic(0, pick == 11u ? 10u : pick);
    tack_shape_track_classic(1, 3); /* FM Bass under */
    tack_shape_track_classic(2, pick);
    song.g[G_RDAMP] = 20;
    song.g[G_CDEPTH] = 32;
    song.g[G_CRATE] = 8;
    song.g[G_RSIZE] = 90;
    tack_shimmer = 0;
    tack_drm_dist = 10;
    tack_drm_comp = 18;
    tack_pong = 0;
    tack_mute &= (uint8_t)~((1u << 0) | (1u << 1) | (1u << 2));
}

/* ---- 6.11 INDUSTRIAL per-track shapers (library presets only; mode/kit/PART untouched) ---- */
static void tack_shape_track_industrial(uint8_t tr, uint8_t pick)
{
    track_t *t = &trk[tr & 3u];
    pick %= TACK_IND_N;
    switch (pick) {
    case 0: /* Scrap Hit — metal scrap as tone */
        set_engine_of(t, 11); apply_preset_to(t, 3); /* METAL */
        t->p[P_ATK] = 0; t->p[P_DEC] = 22; t->p[P_SUS] = 6; t->p[P_REL] = 28;
        t->p[P_DIST] = 36; t->p[P_CHOR] = 2; t->p[P_REV] = 22; t->p[P_DLY] = 8;
        t->p[P_LEVEL] = 100; break;
    case 1: /* Foundry — heavy low grit */
        set_engine_of(t, 0); apply_preset_to(t, 2); /* SQR BASS */
        t->p[P_E0] = 1; t->p[P_E1] = 8; t->p[P_E4] = 28; t->p[P_E5] = 88; t->p[P_E6] = 48;
        t->p[P_ATK] = 4; t->p[P_DEC] = 70; t->p[P_SUS] = 50; t->p[P_REL] = 55;
        t->p[P_DIST] = 42; t->p[P_CHOR] = 0; t->p[P_REV] = 18; t->p[P_DLY] = 6;
        t->p[P_LEVEL] = 118; break;
    case 2: /* Cold Drone — cold sustained industrial bed */
        set_engine_of(t, 11); apply_preset_to(t, 0); /* WIND shaped cold */
        t->p[P_ATK] = 90; t->p[P_DEC] = 80; t->p[P_SUS] = 120; t->p[P_REL] = 126;
        t->p[P_DIST] = 18; t->p[P_CHOR] = 8; t->p[P_REV] = 70; t->p[P_DLY] = 40;
        t->p[P_LRATE] = 4; t->p[P_M1SRC] = 1; t->p[P_M1DST] = 2; t->p[P_M1AMT] = 16;
        t->p[P_LEVEL] = 58; break;
    case 3: /* Dist Stab — crushed factory stab */
        set_engine_of(t, 0); apply_preset_to(t, 8); /* PLUCK */
        t->p[P_E0] = 0; t->p[P_E1] = 6; t->p[P_E4] = 70; t->p[P_E5] = 60;
        t->p[P_ATK] = 0; t->p[P_DEC] = 18; t->p[P_SUS] = 4; t->p[P_REL] = 16;
        t->p[P_DIST] = 48; t->p[P_CHOR] = 0; t->p[P_REV] = 10; t->p[P_DLY] = 4;
        t->p[P_LEVEL] = 112; break;
    case 4: /* Factory Air — plant ambience (not soft Ambient) */
        set_engine_of(t, 11); apply_preset_to(t, 1); /* RAIN → dry plant hiss */
        t->p[P_ATK] = 70; t->p[P_DEC] = 90; t->p[P_SUS] = 110; t->p[P_REL] = 120;
        t->p[P_DIST] = 22; t->p[P_CHOR] = 4; t->p[P_REV] = 88; t->p[P_DLY] = 50;
        t->p[P_LEVEL] = 48; break;
    case 5: /* Iron Plate — bowed / plate metal */
        set_engine_of(t, 9); apply_preset_to(t, 3); /* BOWED METAL (PHYS) */
        t->p[P_ATK] = 8; t->p[P_DEC] = 80; t->p[P_SUS] = 70; t->p[P_REL] = 100;
        t->p[P_DIST] = 16; t->p[P_CHOR] = 10; t->p[P_REV] = 55; t->p[P_DLY] = 22;
        t->p[P_LEVEL] = 86; break;
    case 6: /* Pipe Clang — resonant scrap pipe */
        set_engine_of(t, 11); apply_preset_to(t, 3); /* METAL */
        t->p[P_ATK] = 0; t->p[P_DEC] = 45; t->p[P_SUS] = 12; t->p[P_REL] = 70;
        t->p[P_DIST] = 28; t->p[P_CHOR] = 6; t->p[P_REV] = 48; t->p[P_DLY] = 28;
        t->p[P_LEVEL] = 94; break;
    case 7: /* Rust Saw — corroded saw */
        set_engine_of(t, 0); apply_preset_to(t, 0); /* SAW */
        t->p[P_E0] = 0; t->p[P_E1] = 30; t->p[P_E4] = 55; t->p[P_E5] = 72; t->p[P_E6] = 36;
        t->p[P_ATK] = 2; t->p[P_DEC] = 55; t->p[P_SUS] = 65; t->p[P_REL] = 48;
        t->p[P_DIST] = 40; t->p[P_CHOR] = 4; t->p[P_REV] = 16; t->p[P_DLY] = 10;
        t->p[P_LEVEL] = 104; break;
    case 8: /* Press Stamp — machine stamp hit */
        set_engine_of(t, 11); apply_preset_to(t, 2); /* ARCADE */
        t->p[P_ATK] = 0; t->p[P_DEC] = 14; t->p[P_SUS] = 0; t->p[P_REL] = 12;
        t->p[P_DIST] = 44; t->p[P_CHOR] = 0; t->p[P_REV] = 8; t->p[P_DLY] = 2;
        t->p[P_LEVEL] = 116; break;
    case 9: /* Slag Bass — molten low end */
        set_engine_of(t, 0); apply_preset_to(t, 7); /* SUB-ish */
        t->p[P_E0] = 1; t->p[P_E4] = 22; t->p[P_E5] = 95; t->p[P_E6] = 55;
        t->p[P_ATK] = 0; t->p[P_DEC] = 60; t->p[P_SUS] = 35; t->p[P_REL] = 40;
        t->p[P_DIST] = 38; t->p[P_CHOR] = 0; t->p[P_REV] = 10; t->p[P_DLY] = 4;
        t->p[P_LEVEL] = 120; break;
    case 10: /* Steam Hiss — vent / valve */
        set_engine_of(t, 11); apply_preset_to(t, 0); /* WIND */
        t->p[P_ATK] = 20; t->p[P_DEC] = 50; t->p[P_SUS] = 40; t->p[P_REL] = 60;
        t->p[P_DIST] = 12; t->p[P_CHOR] = 2; t->p[P_REV] = 40; t->p[P_DLY] = 30;
        t->p[P_LRATE] = 22; t->p[P_M1SRC] = 1; t->p[P_M1DST] = 5; t->p[P_M1AMT] = 36;
        t->p[P_LEVEL] = 62; break;
    default: /* 11 PLANT LIFT~ — industrial lift bed */
        set_engine_of(t, 0); apply_preset_to(t, 1);
        t->p[P_E0] = 0; t->p[P_E1] = 24; t->p[P_E4] = 40; t->p[P_E5] = 35;
        t->p[P_ATK] = 50; t->p[P_DEC] = 70; t->p[P_SUS] = 95; t->p[P_REL] = 100;
        t->p[P_DIST] = 20; t->p[P_CHOR] = 14; t->p[P_REV] = 55; t->p[P_DLY] = 28;
        t->p[P_LEVEL] = 72; break;
    }
    tack_level[tr & 3u] = t->p[P_LEVEL];
}

static void tack_apply_industrial_bank(uint8_t pick)
{
    /* Industrial band — metals/noise/foundry; style/kit/PART untouched. */
    pick %= TACK_IND_N;
    /* Comp gets pick (lift→cold drone bed); bass slag; air matches pick or factory. */
    tack_shape_track_industrial(0, pick == 11u ? 2u : (pick == 9u ? 7u : pick));
    tack_shape_track_industrial(1, 9); /* Slag Bass under */
    if (pick == 2u || pick == 4u || pick == 10u || pick == 11u)
        tack_shape_track_industrial(2, pick);
    else if (pick == 0u || pick == 3u || pick == 8u)
        tack_shape_track_industrial(2, 4); /* Factory Air cushion under hits */
    else
        tack_shape_track_industrial(2, pick);
    song.g[G_RDAMP] = 35;
    song.g[G_CDEPTH] = 10;
    song.g[G_CRATE] = 6;
    song.g[G_RSIZE] = 78;
    tack_shimmer = 0;
    tack_drm_dist = 34;
    tack_drm_comp = 36;
    tack_pong = 0;
    tack_mute &= (uint8_t)~((1u << 0) | (1u << 1) | (1u << 2));
}

/* ---- 6.18 JUNO per-track shapers (library presets only; mode/kit/PART untouched) ---- */
static void tack_shape_track_juno(uint8_t tr, uint8_t pick)
{
    /* ANALOG engine: warm poly + BBD-ish chorus. Never DX7/FM6, never Industrial noise. */
    track_t *t = &trk[tr & 3u];
    pick %= TACK_JUNO_N;
    switch (pick) {
    case 0: /* Juno Pad — soft poly pad, heavy chorus shimmer */
        set_engine_of(t, 0); apply_preset_to(t, 1); /* SOFT PAD */
        t->p[P_E0] = 0; t->p[P_E1] = 22; t->p[P_E4] = 52; t->p[P_E5] = 22;
        t->p[P_ATK] = 55; t->p[P_DEC] = 80; t->p[P_SUS] = 110; t->p[P_REL] = 100;
        t->p[P_DIST] = 2; t->p[P_CHOR] = 78; t->p[P_REV] = 48; t->p[P_DLY] = 28;
        t->p[P_LEVEL] = 72; break;
    case 1: /* Chorus Str — strings + wide BBD */
        set_engine_of(t, 0); apply_preset_to(t, 11); /* STRINGS */
        t->p[P_E0] = 0; t->p[P_E1] = 18; t->p[P_E4] = 58; t->p[P_E5] = 20;
        t->p[P_ATK] = 70; t->p[P_DEC] = 85; t->p[P_SUS] = 115; t->p[P_REL] = 110;
        t->p[P_DIST] = 0; t->p[P_CHOR] = 88; t->p[P_REV] = 55; t->p[P_DLY] = 32;
        t->p[P_LEVEL] = 68; break;
    case 2: /* Soft Brass — 80s poly brass */
        set_engine_of(t, 0); apply_preset_to(t, 9); /* BRASS */
        t->p[P_E0] = 0; t->p[P_E1] = 14; t->p[P_E4] = 70; t->p[P_E5] = 32;
        t->p[P_ATK] = 28; t->p[P_DEC] = 55; t->p[P_SUS] = 90; t->p[P_REL] = 55;
        t->p[P_DIST] = 4; t->p[P_CHOR] = 62; t->p[P_REV] = 36; t->p[P_DLY] = 18;
        t->p[P_LEVEL] = 92; break;
    case 3: /* Juno Bass — warm square/sub under */
        set_engine_of(t, 0); apply_preset_to(t, 2); /* SQR BASS */
        t->p[P_E0] = 1; t->p[P_E1] = 8; t->p[P_E4] = 34; t->p[P_E5] = 55; t->p[P_E6] = 28;
        t->p[P_ATK] = 0; t->p[P_DEC] = 50; t->p[P_SUS] = 70; t->p[P_REL] = 40;
        t->p[P_DIST] = 6; t->p[P_CHOR] = 18; t->p[P_REV] = 12; t->p[P_DLY] = 6;
        t->p[P_LEVEL] = 116; break;
    case 4: /* Warm Saw — classic Juno saw lead */
        set_engine_of(t, 0); apply_preset_to(t, 0); /* SAW LEAD */
        t->p[P_E0] = 0; t->p[P_E1] = 20; t->p[P_E4] = 78; t->p[P_E5] = 36;
        t->p[P_ATK] = 6; t->p[P_DEC] = 55; t->p[P_SUS] = 85; t->p[P_REL] = 50;
        t->p[P_DIST] = 4; t->p[P_CHOR] = 70; t->p[P_REV] = 28; t->p[P_DLY] = 16;
        t->p[P_LEVEL] = 96; break;
    case 5: /* Squ Lead — square lead with chorus */
        set_engine_of(t, 0); apply_preset_to(t, 2); /* SQR shaped as lead */
        t->p[P_E0] = 1; t->p[P_E1] = 16; t->p[P_E4] = 82; t->p[P_E5] = 30;
        t->p[P_ATK] = 2; t->p[P_DEC] = 45; t->p[P_SUS] = 75; t->p[P_REL] = 42;
        t->p[P_DIST] = 2; t->p[P_CHOR] = 66; t->p[P_REV] = 24; t->p[P_DLY] = 14;
        t->p[P_LEVEL] = 98; break;
    case 6: /* Org Choir — organ-ish held poly */
        set_engine_of(t, 0); apply_preset_to(t, 5); /* SINE KEY → organ feel */
        t->p[P_E0] = 3; t->p[P_E1] = 10; t->p[P_E4] = 64; t->p[P_E5] = 18;
        t->p[P_ATK] = 0; t->p[P_DEC] = 20; t->p[P_SUS] = 120; t->p[P_REL] = 35;
        t->p[P_DIST] = 0; t->p[P_CHOR] = 74; t->p[P_REV] = 30; t->p[P_DLY] = 12;
        t->p[P_LEVEL] = 90; break;
    case 7: /* Poly Pluck — short pluck + chorus */
        set_engine_of(t, 0); apply_preset_to(t, 8); /* PLUCK */
        t->p[P_E0] = 0; t->p[P_E1] = 12; t->p[P_E4] = 88; t->p[P_E5] = 28;
        t->p[P_ATK] = 0; t->p[P_DEC] = 32; t->p[P_SUS] = 12; t->p[P_REL] = 28;
        t->p[P_DIST] = 2; t->p[P_CHOR] = 58; t->p[P_REV] = 32; t->p[P_DLY] = 20;
        t->p[P_LEVEL] = 100; break;
    case 8: /* Poly Keys — soft EP / keys */
        set_engine_of(t, 0); apply_preset_to(t, 5); /* SINE KEY */
        t->p[P_E0] = 0; t->p[P_E1] = 14; t->p[P_E4] = 72; t->p[P_E5] = 24;
        t->p[P_ATK] = 4; t->p[P_DEC] = 70; t->p[P_SUS] = 50; t->p[P_REL] = 60;
        t->p[P_DIST] = 0; t->p[P_CHOR] = 64; t->p[P_REV] = 38; t->p[P_DLY] = 18;
        t->p[P_LEVEL] = 94; break;
    case 9: /* PWM Soft — PWM strings / soft pulse */
        set_engine_of(t, 0); apply_preset_to(t, 3); /* PWM STR */
        t->p[P_E0] = 4; t->p[P_E1] = 26; t->p[P_E4] = 60; t->p[P_E5] = 26;
        t->p[P_ATK] = 40; t->p[P_DEC] = 75; t->p[P_SUS] = 100; t->p[P_REL] = 90;
        t->p[P_DIST] = 0; t->p[P_LRATE] = 14; t->p[P_M1SRC] = 1; t->p[P_M1DST] = 2;
        t->p[P_M1AMT] = 36; t->p[P_CHOR] = 82; t->p[P_REV] = 42; t->p[P_DLY] = 24;
        t->p[P_LEVEL] = 74; break;
    case 10: /* Night Pad — darker dusk pad */
        set_engine_of(t, 0); apply_preset_to(t, 1); /* SOFT PAD */
        t->p[P_E0] = 0; t->p[P_E1] = 28; t->p[P_E4] = 40; t->p[P_E5] = 16;
        t->p[P_ATK] = 85; t->p[P_DEC] = 90; t->p[P_SUS] = 118; t->p[P_REL] = 118;
        t->p[P_DIST] = 0; t->p[P_CHOR] = 86; t->p[P_REV] = 70; t->p[P_DLY] = 40;
        t->p[P_LEVEL] = 60; break;
    default: /* 11 JUNO LIFT~ — chorus lift bed */
        set_engine_of(t, 0); apply_preset_to(t, 11); /* STRINGS lift */
        t->p[P_E0] = 0; t->p[P_E1] = 30; t->p[P_E4] = 68; t->p[P_E5] = 24;
        t->p[P_ATK] = 35; t->p[P_DEC] = 70; t->p[P_SUS] = 105; t->p[P_REL] = 95;
        t->p[P_DIST] = 0; t->p[P_CHOR] = 92; t->p[P_REV] = 58; t->p[P_DLY] = 34;
        t->p[P_LEVEL] = 78; break;
    }
    tack_level[tr & 3u] = t->p[P_LEVEL];
}

static void tack_apply_juno_bank(uint8_t pick)
{
    /* Juno band — ANALOG poly + BBD chorus depth; style/kit/PART untouched. */
    pick %= TACK_JUNO_N;
    /* Comp gets pick (lift→pad bed); bass Juno Bass; air matches pick or pad. */
    tack_shape_track_juno(0, pick == 11u ? 0u : (pick == 3u ? 4u : pick));
    tack_shape_track_juno(1, 3); /* Juno Bass under */
    if (pick == 0u || pick == 1u || pick == 9u || pick == 10u || pick == 11u)
        tack_shape_track_juno(2, pick);
    else if (pick == 4u || pick == 5u || pick == 7u)
        tack_shape_track_juno(2, 0); /* pad cushion under leads/plucks */
    else
        tack_shape_track_juno(2, pick);
    song.g[G_RDAMP] = 18;
    song.g[G_CDEPTH] = 72; /* BBD chorus shimmer — Juno signature */
    song.g[G_CRATE] = 12;
    song.g[G_RSIZE] = 88;
    tack_shimmer = 0;
    tack_drm_dist = 8;
    tack_drm_comp = 16;
    tack_pong = 0;
    tack_mute &= (uint8_t)~((1u << 0) | (1u << 1) | (1u << 2));
}

static void tack_apply_sound_bank(uint8_t bank)
{
    /* Library = sounds only. NEVER changes style/mode. NEVER clears a held kit. */
    uint8_t style_keep = tack_style_idx;
    tack_snd_bank = bank % TACK_LIB_N;
    song.g[G_CLOCK] = 0;
    tack_mute &= (uint8_t)~(1u << 3);
    switch (tack_snd_bank) {
    case 0: /* KITS — voice/drum entry only; style unchanged */
        tack_mute &= (uint8_t)~(1u << 3);
        set_engine_of(&trk[3], ENGI_DRUM);
        apply_preset_to(&trk[3], 4);
        tack_level[3] = trk[3].p[P_LEVEL] = 104;
        tack_mute &= (uint8_t)~(1u << 3);
        tack_drm_dist = 16; tack_drm_comp = 28;
        tack_apply_drum_bus();
        tack_snd_cursor = 0;
        tack_flash("BANK KITS");
        break;
    case 1: /* AMBIENT — soft/zen pads; style/kit stay */
        tack_snd_bank = 1;
        tack_amb_tex = tack_snd_pick % TACK_AMB_SOFT_N;
        tack_apply_amb_texture();
        tack_snd_cursor = 1;
        tack_flash(tack_lib_sound_name(1, tack_snd_pick));
        break;
    case 2: /* DARK AMB — Oberheim huge; style/kit stay */
        tack_snd_bank = 2;
        tack_amb_tex = tack_snd_pick % TACK_AMB_TEX_N;
        tack_apply_amb_texture();
        tack_snd_cursor = 2;
        tack_flash(tack_lib_sound_name(2, tack_snd_pick));
        break;
    case 3: /* SCI-FI — F/X space; style/kit stay */
        song.g[G_DMIX] = 90; song.g[G_RSIZE] = 118; song.g[G_RDAMP] = 12;
        tack_snd_bank = 3;
        tack_amb_tex = tack_snd_pick % TACK_AMB_TEX_N;
        tack_apply_amb_texture();
        tack_snd_cursor = 3;
        tack_flash(tack_lib_sound_name(3, tack_snd_pick));
        break;
    case 4: /* 303 / ACID — bass lane star; style/kit unchanged */
        set_engine_of(&trk[1], 0); apply_preset_to(&trk[1], 4); /* ACID */
        set_engine_of(&trk[0], 0); apply_preset_to(&trk[0], 4);
        trk[1].p[P_DIST] = 28;
        trk[1].p[P_DEC] = 40;
        trk[1].p[P_SUS] = 20;
        tack_level[1] = trk[1].p[P_LEVEL] = 118;
        tack_level[0] = trk[0].p[P_LEVEL] = 72;  /* duck comp under bass */
        tack_mute_sel = 1; /* focus bass lane for LFO track assign */
        tack_mute &= (uint8_t)~(1u << 1);
        tack_snd_cursor = 4;
        tack_flash("BANK 303 BASS");
        break;
    case 5: /* ZEN — calm bed voices only; style unchanged */
        tack_snd_bank = 5;
        if (tack_amb_tex >= TACK_AMB_TEX_N) tack_amb_tex = 6; /* Zen Air default */
        tack_apply_amb_texture();
        tack_level[3] = trk[3].p[P_LEVEL] = 48;
        tack_snd_cursor = 5;
        tack_flash("BANK ZEN");
        break;
    case 6: /* TECHNO — club techno; style/kit stay */
        tack_snd_bank = 6;
        tack_apply_techno_bank(tack_snd_pick);
        tack_snd_cursor = 6;
        tack_flash(tack_lib_sound_name(6, tack_snd_pick));
        break;
    case 7: /* CLASSIC — DX7 FM; style/kit stay */
        tack_snd_bank = 7;
        tack_apply_classic_bank(tack_snd_pick);
        tack_snd_cursor = 7;
        tack_flash(tack_lib_sound_name(7, tack_snd_pick));
        break;
    case 8: /* INDUSTRIAL — foundry/metals; style/kit/PART stay */
        tack_snd_bank = 8;
        tack_apply_industrial_bank(tack_snd_pick);
        tack_snd_cursor = 8;
        tack_flash(tack_lib_sound_name(8, tack_snd_pick));
        break;
    case 9: /* JUNO — poly analog + BBD chorus; style/kit/PART stay */
        tack_snd_bank = 9;
        tack_apply_juno_bank(tack_snd_pick);
        tack_snd_cursor = 9;
        tack_flash(tack_lib_sound_name(9, tack_snd_pick));
        break;
    default:
        tack_snd_bank = 1;
        tack_apply_amb_texture();
        tack_snd_cursor = 1;
        tack_flash("BANK AMBIENT");
        break;
    }
    /* Guard: library must never have moved style. */
    if (tack_style_idx != style_keep) tack_style_idx = style_keep;
    /* 6.4: library apply → catalog index on voice lanes (kits keep drum pick). */
    if (tack_snd_bank == 0u) {
        /* drum catalog lives on track 3; leave voice picks alone */
    } else {
        uint8_t pk = tack_snd_pick % tack_lib_sound_n(tack_snd_bank);
        tack_trk_lib_pick[0] = tack_trk_lib_pick[1] = tack_trk_lib_pick[2] = pk;
    }
    /* Re-assert held kit so Ambient/303 banks do not orphan CR-78 etc. */
    if (tack_kit_sel <= 3u) {
        apply_preset_to(&trk[3], TACK_KIT_PRESET[tack_kit_sel]);
        tack_beat_kit = (tack_kit_sel >= 2u) ? (uint8_t)(tack_kit_sel - 1u) : 0u;
        tack_apply_drum_bus();
    }
    {
        uint32_t i;
        for (i = 0; i < 4u; i++)
            if (!(tack_mute & (1u << i)))
                tack_level[i] = trk[i].p[P_LEVEL];
    }
    /* apply_preset_to resets DIST/CHOR/DLY/REV to FX_DEF — re-push wet masters */
    tack_apply_fx_amounts();
    if (tack_shimmer) tack_apply_shimmer();
    tack_apply_drum_bus();
    tack_assert_poly_chords();
    tack_restart();
    /* 6.12 soft — bank/pick in sig */
}

/* Apply one library sound to ONE track (LFO: Track → Sound). EDIT uses full-bank path. */
static void tack_apply_lib_sound_track(uint8_t tr, uint8_t bank, uint8_t pick)
{
    uint8_t tex;
    tr &= 3u;
    bank = (uint8_t)(bank % TACK_LIB_N);
    pick = (uint8_t)(pick % tack_lib_sound_n(bank));
    tack_snd_pick = pick;

    if (bank == 0u) {
        /* KITS — drums only; other tracks flash hint */
        if (tr != 3u) {
            tack_flash("DRUM:KITS");
            return;
        }
        tack_apply_classic_kit(pick);
        tack_trk_lib_pick[3] = pick;
        tack_flash2("KIT ", TACK_KIT_TAG[pick]);
        return;
    }
    if (bank == 4u) {
        /* 303/ACID — prefer bass; allow any non-drum track */
        if (tr == 3u) {
            tack_flash("NO DRUM ACID");
            return;
        }
        set_engine_of(&trk[tr], 0);
        apply_preset_to(&trk[tr], TACK_ACID_PRE[pick]);
        if (tr == 1u) {
            trk[tr].p[P_DIST] = 28;
            trk[tr].p[P_DEC] = 40;
            trk[tr].p[P_SUS] = 20;
            tack_level[tr] = trk[tr].p[P_LEVEL] = 118;
        } else {
            tack_level[tr] = trk[tr].p[P_LEVEL];
        }
        tack_mute &= (uint8_t)~(1u << tr);
        tack_trk_lib_pick[tr] = pick;
        if (tr == 2u) tack_air_user = 1;
        tack_apply_fx_amounts();
        tack_assert_poly_chords();
        tack_flash2("TRK ", TACK_ACID_NAME[pick]);
        return;
    }
    if (bank == 6u) {
        /* TECHNO — club tones on voice lanes */
        if (tr == 3u) { tack_flash("USE KITS"); return; }
        tack_shape_track_techno(tr, pick);
        tack_mute &= (uint8_t)~(1u << tr);
        tack_trk_lib_pick[tr] = pick;
        if (tr == 2u) tack_air_user = 1;
        tack_apply_fx_amounts();
        tack_assert_poly_chords();
        tack_flash2("TRK ", TACK_TECHNO_NAME[pick]);
        return;
    }
    if (bank == 7u) {
        /* CLASSIC DX7 — FM6 catalog on voice lanes */
        if (tr == 3u) { tack_flash("USE KITS"); return; }
        tack_shape_track_classic(tr, pick);
        tack_mute &= (uint8_t)~(1u << tr);
        tack_trk_lib_pick[tr] = pick;
        if (tr == 2u) tack_air_user = 1;
        tack_apply_fx_amounts();
        tack_assert_poly_chords();
        tack_flash2("TRK ", TACK_CLASSIC_NAME[pick]);
        return;
    }
    if (bank == 8u) {
        /* INDUSTRIAL — metals/foundry on voice lanes */
        if (tr == 3u) { tack_flash("USE KITS"); return; }
        tack_shape_track_industrial(tr, pick);
        tack_mute &= (uint8_t)~(1u << tr);
        tack_trk_lib_pick[tr] = pick;
        if (tr == 2u) tack_air_user = 1;
        tack_apply_fx_amounts();
        tack_assert_poly_chords();
        tack_flash2("TRK ", TACK_IND_NAME[pick]);
        return;
    }
    if (bank == 9u) {
        /* JUNO — poly analog + chorus on voice lanes */
        if (tr == 3u) { tack_flash("USE KITS"); return; }
        tack_shape_track_juno(tr, pick);
        tack_mute &= (uint8_t)~(1u << tr);
        tack_trk_lib_pick[tr] = pick;
        if (tr == 2u) tack_air_user = 1;
        tack_apply_fx_amounts();
        tack_assert_poly_chords();
        tack_flash2("TRK ", TACK_JUNO_NAME[pick]);
        return;
    }
    /* Library presets onto one track — catalog follows bank (never BELL). */
    tex = tack_lib_tex_from_pick(bank, pick);
    if (tr == 3u) {
        tack_flash("USE KITS");
        return;
    }
    {
        uint8_t bright = (bank == 1u || bank == 5u)
            ? (tex >= (TACK_AMB_SOFT_N - 1u))
            : (tex == 11u);
        if (bank == 2u) {
            /* Dark Ambient — Oberheim huge on this lane */
            set_engine_of(&trk[tr], 0);
            apply_preset_to(&trk[tr], bright ? 1 : (tex == 7u ? 9 : (tex == 1u || tex == 8u ? 11 : 1)));
            if (bright) tack_shape_track_ob_huge(tr, 1, 0, 88, 68, 26);
            else if (tex == 0) tack_shape_track_ob_huge(tr, 0, 0, 92, 42, 28);
            else if (tex == 1) tack_shape_track_ob_huge(tr, 0, 0, 100, 38, 22);
            else if (tex == 2) { tack_shape_track_ob_huge(tr, 0, 0, 84, 44, 30); trk[tr].p[P_LEVEL] = 74; tack_level[tr] = 74; }
            else if (tex == 3) tack_shape_track_ob_huge(tr, 0, 4, 96, 40, 26);
            else if (tex == 4) { tack_shape_track_ob_huge(tr, 0, 0, 78, 46, 20); trk[tr].p[P_CHOR] = 80; }
            else if (tex == 5) { tack_shape_track_ob_huge(tr, 0, 0, 70, 28, 18); trk[tr].p[P_LEVEL] = 62; tack_level[tr] = 62; }
            else if (tex == 6) { tack_shape_track_ob_huge(tr, 0, 0, 86, 34, 36); trk[tr].p[P_LRATE] = 3; trk[tr].p[P_M1AMT] = 36; }
            else if (tex == 7) tack_shape_track_ob_huge(tr, 0, 0, 74, 48, 24);
            else if (tex == 8) { tack_shape_track_ob_huge(tr, 0, 0, 90, 36, 22); trk[tr].p[P_REV] = 124; }
            else if (tex == 9) { tack_shape_track_ob_huge(tr, 0, 0, 80, 30, 42); trk[tr].p[P_LRATE] = 4; trk[tr].p[P_M1AMT] = 40; }
            else { tack_shape_track_ob_huge(tr, 0, 0, 108, 40, 26); trk[tr].p[P_CHOR] = 88; }
        } else if (bank == 3u) {
            /* Sci-Fi F/X space on this lane */
            if (bright) {
                set_engine_of(&trk[tr], 0); apply_preset_to(&trk[tr], 1);
                tack_shape_track_fx_space(tr, 1, 0, 28, 70, 18, 1);
            } else if (tex == 7) {
                set_engine_of(&trk[tr], 11); apply_preset_to(&trk[tr], 1);
                trk[tr].p[P_ATK] = 90; trk[tr].p[P_REL] = 124;
                trk[tr].p[P_REV] = 118; trk[tr].p[P_DLY] = 80;
                trk[tr].p[P_LRATE] = 12; trk[tr].p[P_M1SRC] = 1; trk[tr].p[P_M1DST] = 5; trk[tr].p[P_M1AMT] = 40;
                trk[tr].p[P_LEVEL] = 42; tack_level[tr] = 42;
            } else if (tex == 3) {
                set_engine_of(&trk[tr], 2); apply_preset_to(&trk[tr], 5); /* WIRE */
                trk[tr].p[P_ATK] = 100; trk[tr].p[P_REL] = 126;
                trk[tr].p[P_CHOR] = 48; trk[tr].p[P_REV] = 120; trk[tr].p[P_DLY] = 88;
                trk[tr].p[P_LRATE] = 14; trk[tr].p[P_M1SRC] = 1; trk[tr].p[P_M1DST] = 5; trk[tr].p[P_M1AMT] = 44;
                trk[tr].p[P_LEVEL] = 46; tack_level[tr] = 46;
            } else {
                set_engine_of(&trk[tr], 0); apply_preset_to(&trk[tr], (tex == 0 || tex == 4 || tex == 8) ? 7 : 1);
                if (tex == 0) tack_shape_track_fx_space(tr, 0, 3, 10, 32, 12, 0);
                else if (tex == 1) tack_shape_track_fx_space(tr, 0, 0, 24, 50, 16, 1);
                else if (tex == 2) { tack_shape_track_fx_space(tr, 0, 0, 18, 40, 14, 0); trk[tr].p[P_REV] = 126; trk[tr].p[P_DLY] = 110; }
                else if (tex == 4) { tack_shape_track_fx_space(tr, 0, 3, 6, 26, 8, 0); trk[tr].p[P_LEVEL] = 36; tack_level[tr] = 36; }
                else if (tex == 5) tack_shape_track_fx_space(tr, 0, 4, 30, 48, 22, 1);
                else if (tex == 6) { tack_shape_track_fx_space(tr, 0, 0, 16, 44, 12, 0); trk[tr].p[P_ATK] = 118; trk[tr].p[P_DLY] = 118; }
                else if (tex == 8) { tack_shape_track_fx_space(tr, 0, 0, 40, 22, 30, 0); trk[tr].p[P_LRATE] = 4; trk[tr].p[P_M1AMT] = 32; }
                else if (tex == 9) { tack_shape_track_fx_space(tr, 0, 0, 34, 52, 18, 1); trk[tr].p[P_LRATE] = 20; trk[tr].p[P_M1AMT] = 56; }
                else tack_shape_track_fx_space(tr, 0, 2, 12, 36, 10, 0);
            }
        } else {
            /* Ambient / Zen — soft pads (6.13 soft catalog) */
            tack_amb_apply_soft_pick(tr, (uint8_t)(tex % TACK_AMB_SOFT_N));
            bright = ((tex % TACK_AMB_SOFT_N) >= (TACK_AMB_SOFT_N - 1u));
        }
        if (bank == 5u && !bright) {
            if (trk[tr].p[P_LEVEL] > 42) trk[tr].p[P_LEVEL] = 42;
            tack_level[tr] = trk[tr].p[P_LEVEL];
            if (trk[tr].p[P_REV] < 110) trk[tr].p[P_REV] = 110;
        }
    }
    tack_amb_tex = tex;
    tack_mute &= (uint8_t)~(1u << tr);
    tack_trk_lib_pick[tr] = pick; /* 6.4: remember catalog index for this lane */
    if (tr == 2u) tack_air_user = 1; /* 6.14 spare air — style must not stomp */
    tack_apply_fx_amounts();
    if (tack_shimmer) tack_apply_shimmer();
    tack_assert_poly_chords();
    tack_flash(tack_lib_sound_name(bank, pick));
}

static void tack_apply_edit_sound(uint8_t bank, uint8_t pick)
{
    /* EDIT: library preset → band. Never touches style/mode/kit axes. */
    bank = (uint8_t)(bank % TACK_LIB_N);
    pick = (uint8_t)(pick % tack_lib_sound_n(bank));
    tack_snd_pick = pick;
    if (bank == 0u) {
        tack_apply_sound_bank(0);
        tack_apply_classic_kit(pick);
        tack_trk_lib_pick[3] = pick;
        return;
    }
    if (bank == 4u) {
        tack_apply_sound_bank(4);
        set_engine_of(&trk[1], 0); apply_preset_to(&trk[1], TACK_ACID_PRE[pick]);
        tack_level[1] = trk[1].p[P_LEVEL];
        tack_trk_lib_set_all_voices(pick);
        tack_flash2("303 ", TACK_ACID_NAME[pick]);
        return;
    }
    if (bank == 6u) {
        tack_snd_bank = 6; tack_snd_cursor = 6; tack_snd_pick = pick;
        tack_apply_techno_bank(pick);
        tack_trk_lib_set_all_voices(pick);
        if (tack_kit_sel <= 3u) {
            apply_preset_to(&trk[3], TACK_KIT_PRESET[tack_kit_sel]);
            tack_beat_kit = (tack_kit_sel >= 2u) ? (uint8_t)(tack_kit_sel - 1u) : 0u;
            tack_apply_drum_bus();
        }
        tack_apply_fx_amounts();
        tack_assert_poly_chords();
        tack_flash(TACK_TECHNO_NAME[pick]);
        return;
    }
    if (bank == 7u) {
        tack_snd_bank = 7; tack_snd_cursor = 7; tack_snd_pick = pick;
        tack_apply_classic_bank(pick);
        tack_trk_lib_set_all_voices(pick);
        if (tack_kit_sel <= 3u) {
            apply_preset_to(&trk[3], TACK_KIT_PRESET[tack_kit_sel]);
            tack_beat_kit = (tack_kit_sel >= 2u) ? (uint8_t)(tack_kit_sel - 1u) : 0u;
            tack_apply_drum_bus();
        }
        tack_apply_fx_amounts();
        tack_assert_poly_chords();
        tack_flash(TACK_CLASSIC_NAME[pick]);
        return;
    }
    if (bank == 8u) {
        tack_snd_bank = 8; tack_snd_cursor = 8; tack_snd_pick = pick;
        tack_apply_industrial_bank(pick);
        tack_trk_lib_set_all_voices(pick);
        if (tack_kit_sel <= 3u) {
            apply_preset_to(&trk[3], TACK_KIT_PRESET[tack_kit_sel]);
            tack_beat_kit = (tack_kit_sel >= 2u) ? (uint8_t)(tack_kit_sel - 1u) : 0u;
            tack_apply_drum_bus();
        }
        tack_apply_fx_amounts();
        tack_assert_poly_chords();
        tack_flash(TACK_IND_NAME[pick]);
        return;
    }
    if (bank == 9u) {
        tack_snd_bank = 9; tack_snd_cursor = 9; tack_snd_pick = pick;
        tack_apply_juno_bank(pick);
        tack_trk_lib_set_all_voices(pick);
        if (tack_kit_sel <= 3u) {
            apply_preset_to(&trk[3], TACK_KIT_PRESET[tack_kit_sel]);
            tack_beat_kit = (tack_kit_sel >= 2u) ? (uint8_t)(tack_kit_sel - 1u) : 0u;
            tack_apply_drum_bus();
        }
        tack_apply_fx_amounts();
        tack_assert_poly_chords();
        tack_flash(TACK_JUNO_NAME[pick]);
        return;
    }
    /* Ambient / Dark / Sci-Fi / Zen — texture from this library only */
    tack_amb_tex = tack_lib_tex_from_pick(bank, pick);
    tack_snd_bank = bank;
    tack_snd_cursor = bank;
    tack_apply_amb_texture();
    tack_trk_lib_set_all_voices(pick); /* 6.4: band apply → all voice lanes show this catalog name */
    if (tack_kit_sel <= 3u) {
        apply_preset_to(&trk[3], TACK_KIT_PRESET[tack_kit_sel]);
        tack_beat_kit = (tack_kit_sel >= 2u) ? (uint8_t)(tack_kit_sel - 1u) : 0u;
        tack_apply_drum_bus();
    }
    tack_flash(tack_lib_sound_name(bank, pick));
}


static void tack_cycle_engine(int32_t dir)
{
    track_t *t = &trk[tack_mute_sel & 3u];
    uint32_t e = t->eng_req % NENGINES;
    if ((tack_mute_sel & 3u) == 3u) {
        uint8_t np = (uint8_t)ENGINES[ENGI_DRUM]->npresets;
        uint8_t p = (uint8_t)((t->preset + (dir > 0 ? 1 : np - 1)) % (np ? np : 1));
        set_engine_of(t, ENGI_DRUM);
        apply_preset_to(t, p);
        if (!(tack_mute & (1u << 3))) {
            tack_level[3] = t->p[P_LEVEL];
            if (tack_level[3] < 40) { tack_level[3] = 100; t->p[P_LEVEL] = 100; }
        }
        tack_flash2("KIT ", tack_kit_label(t));
        tack_apply_fx_amounts();
        tack_apply_drum_bus();
        return;
    }
    e = eng_step(e, dir);
    set_engine_of(t, e);
    tack_level[tack_mute_sel & 3u] = t->p[P_LEVEL];
    tack_apply_fx_amounts();
    if (tack_shimmer) tack_apply_shimmer();
    tack_assert_poly_chords(); /* keep chord beds poly after ALG change */
    tack_flash2("ALG ", tack_eng_name((uint8_t)e));
}

static void tack_cycle_preset(int32_t dir)
{
    /* 6.3: PRESET encoder = active LIBRARY presets on focused lane.
     * Engine factory banks no longer ignore the chosen library. */
    uint8_t tr = tack_mute_sel & 3u;
    uint8_t n = tack_lib_sound_n(tack_snd_bank);
    int32_t p = (int32_t)tack_snd_pick + (dir > 0 ? 1 : -1);
    if (n == 0u) n = 1u;
    if (p < 0) p = (int32_t)n - 1;
    if (p >= (int32_t)n) p = 0;
    tack_snd_pick = (uint8_t)p;
    tack_apply_lib_sound_track(tr, tack_snd_bank, tack_snd_pick);
    /* Accent survival: EN_PRESET must full-redraw HOME/edit selection chrome. */
}

static void tack_pick_style(uint8_t s)
{
    char msg[24], b[8];
    uint8_t old = tack_style_idx < TACK_STYLE_COUNT ? tack_style_idx : 0;
    int16_t keep_bpm = song.g[G_BPM];
    /* User tempo sticks: only snap to style BPM when still at previous style default. */
    uint8_t user_tempo = (keep_bpm != (int16_t)TACK_STYLE_BPM[old]);
    tack_style_idx = s % TACK_STYLE_COUNT;
    tack_style_cursor = tack_style_idx;
    /* Beatles: warm major-key pop-rock — favor I–vi–IV–V banks via MAJOR mode. */
    if (tack_style_idx == TACK_STYLE_BEATLES) {
        tack_mode_idx = TACK_MODE_MAJOR;
        tack_set_mode_keep(&tack_state, TACK_MODE_MAJOR, tack_slot_owned);
    }
    tack_set_style_keep(&tack_state, tack_style_idx, tack_slot_owned);
    tack_loop = 0;
    {
        uint8_t zi, any = 0;
        for (zi = 0; zi < TACK_SLOTS; zi++) if (tack_slot_owned[zi]) any = 1;
        if (!any) tack_user_chose = 0; /* generated take — Auto opens on HOME */
    }
    tack_apply_style();
    /* Accent survival: style/preset apply must full-redraw selection chrome. */
    song.g[G_CLOCK] = 0;
    if (user_tempo) {
        song.g[G_BPM] = keep_bpm;
        if (song.g[G_BPM] > 140) song.g[G_BPM] = 140;
        if (song.g[G_BPM] < 40) song.g[G_BPM] = 40;
    } else {
        song.g[G_BPM] = (int16_t)TACK_STYLE_BPM[tack_style_idx];
        if (song.g[G_BPM] > 140) song.g[G_BPM] = 140;
    }
    tack_restart();
    fmt_int(b, song.g[G_BPM]);
    str_cpy(msg, TACK_STYLE_NAME[tack_style_idx], sizeof msg);
    tack_str_cat(msg, " ", sizeof msg);
    tack_str_cat(msg, b, sizeof msg);
    tack_flash(msg); /* style name + BPM (style default or kept user tempo) */
}


/* Named FX scenes — independent of style (Dry / Room / Dub / Shimmer). */
static const char *const TACK_FXSC_NAME[4] = {"DRY", "ROOM", "DUB", "SHIMMER"};
static void tack_apply_fx_scene(uint8_t sc)
{
    sc %= 4u;
    tack_fx_scene = sc;
    tack_fx_scene_lock = 1u;
    switch (sc) {
    case 0: /* DRY — truly dry: zero wet/sends/inserts; no leftover wash */
        tack_fx_dly = 0; tack_fx_rev = 0; tack_fx_cho = 0;
        tack_fx_flange = 0; tack_fx_crush = 0; tack_fx_vcho = 0; tack_fx_chaos = 0; tack_fx_tape = 0;
        tack_shimmer = 0; tack_pong = 0;
        song.g[G_RTYPE] = 0; song.g[G_RSIZE] = 36; song.g[G_RDAMP] = 40;
        song.g[G_DMIX] = 0; song.g[G_DFDBK] = 0; song.g[G_DCOLOR] = 40;
        song.g[G_CDEPTH] = 0; song.g[G_CRATE] = 0; song.g[G_DTIME] = 2;
        break;
    case 1: /* ROOM */
        tack_fx_dly = 36; tack_fx_rev = 72; tack_fx_cho = 28; tack_shimmer = 0;
        song.g[G_RTYPE] = 0; song.g[G_RSIZE] = 78; song.g[G_DMIX] = 88;
        break;
    case 2: /* DUB */
        tack_fx_dly = 96; tack_fx_rev = 100; tack_fx_cho = 40; tack_shimmer = 0;
        song.g[G_RTYPE] = 1; song.g[G_RSIZE] = 110; song.g[G_DFDBK] = 92;
        song.g[G_DMIX] = 118; song.g[G_DTIME] = 6;
        break;
    default: /* SHIMMER */
        tack_fx_dly = 48; tack_fx_rev = 110; tack_fx_cho = 72; tack_shimmer = 2;
        song.g[G_RTYPE] = 2; song.g[G_RSIZE] = 118; song.g[G_DMIX] = 100;
        song.g[G_CDEPTH] = 70;
        break;
    }
    tack_apply_fx_amounts(); /* zeros sends + voice inserts when DRY masters are 0 */
    if (sc == 0u) {
        /* Force insert/tape path off again after amounts (no leftover flange wash). */
        tack_fx_flange = 0; tack_fx_crush = 0; tack_fx_vcho = 0; tack_fx_chaos = 0; tack_fx_tape = 0;
        tack_apply_voice_inserts();
        tack_apply_pong(); /* pong off → calm DCOLOR */
    } else if (tack_shimmer) {
        tack_apply_shimmer();
    }
    /* Quiet status: scene name only — no "FX SET" / "FX@BAR" junk overlay. */
    tack_flash(TACK_FXSC_NAME[sc]);
}

/* 6.24 — FX Room: windowed rows + scene chips (calm dirty blit; horse-proof). */
#define TACK_FX_N      14u
#define TACK_FX_VIS    7u
#define TACK_FX_ROW_H  16u
#define TACK_FX_LIST_Y 58u
static const char *const TACK_FXROW[14] = {
    "DELAY", "FDBK", "REVERB", "CHORUS", "SHIMMER",
    "FLANGER", "BIT RED", "V CHO", "CHAOS", "TAPE",
    "DRM DIST", "DRM COMP", "FREEZE", "MORPH"
};
static void tack_fx_row_value(uint8_t i, char *vb, unsigned vbsz, int16_t *val, uint8_t *txt)
{
    *txt = 0; *val = 0; vb[0] = 0;
    switch (i % TACK_FX_N) {
    case 0: *val = (int16_t)tack_fx_dly; break;
    case 1: *val = song.g[G_DFDBK]; break;
    case 2: *val = (int16_t)tack_fx_rev; break;
    case 3: *val = (int16_t)tack_fx_cho; break;
    case 4: *val = (int16_t)tack_shimmer; break;
    case 5: *val = (int16_t)tack_fx_flange; break;
    case 6: *val = (int16_t)tack_fx_crush; break;
    case 7: *val = (int16_t)tack_fx_vcho; break;
    case 8: *val = (int16_t)tack_fx_chaos; break;
    case 9: *val = (int16_t)tack_fx_tape; break;
    case 10: *val = (int16_t)tack_drm_dist; break;
    case 11: *val = (int16_t)tack_drm_comp; break;
    case 12: str_cpy(vb, tack_air_freeze ? "ON" : "OFF", vbsz); *txt = 1; break;
    default: *val = (int16_t)tack_air_morph; break;
    }
}
static uint8_t tack_fx_list_top(void)
{
    uint8_t sel = tack_fx_sel % TACK_FX_N;
    if (sel < (TACK_FX_VIS / 2u)) return 0;
    if (sel + (TACK_FX_VIS / 2u) + 1u > TACK_FX_N)
        return (uint8_t)(TACK_FX_N - TACK_FX_VIS);
    return (uint8_t)(sel - (TACK_FX_VIS / 2u));
}
static void tack_paint_fx_scene_strip(void)
{
    uint8_t si;
    for (si = 0; si < 4u; si++) {
        int32_t x = 8 + (int32_t)si * 56;
        uint8_t on = (si == (tack_fx_scene & 3u));
        uint8_t q = (tack_q_fxsc == si);
        uint16_t fill = on ? ux_mix(T_BG, TACK_COL_LIVE, 58)
            : (q ? ux_mix(T_BG, T_ACCENT, 40) : ux_mix(T_BG, T_THEME, 14));
        uint16_t ink = on ? T_INK : (q ? T_ACCENT : T_DIM);
        lcd_fill((uint32_t)x, 36, 54, 16, fill);
        if (on) lcd_fill((uint32_t)x, 36, 3, 16, TACK_COL_LIVE);
        draw_text_line((uint32_t)x + 6u, 38, 46, &AF_S, TACK_FXSC_NAME[si], ink, fill, 0);
    }
}
static void tack_paint_fx_row(uint8_t i, uint8_t slot)
{
    char vb[8], nb[8];
    int16_t val = 0;
    uint8_t txt = 0;
    uint8_t on = (i == tack_fx_sel);
    uint16_t sig = TACK_COL_LIVE;
    uint16_t fill = on ? ux_mix(T_BG, sig, 58) : T_BG;
    uint16_t ink = on ? T_INK : T_TEXT;
    uint32_t y = TACK_FX_LIST_Y + (uint32_t)slot * TACK_FX_ROW_H;
    tack_fx_row_value(i, vb, sizeof vb, &val, &txt);
    cv_begin(224u, TACK_FX_ROW_H, fill);
    if (on) cv_rect(0, 0, 3, (int32_t)TACK_FX_ROW_H, sig);
    cv_text_on(6, 2, &AF_S, TACK_FXROW[i % TACK_FX_N], ink, fill);
    if (txt) cv_text_r(210, 2, &AF_S, vb, on ? T_INK : T_DIM, fill);
    else { fmt_int(nb, val); cv_text_r(210, 2, &AF_S, nb, on ? T_INK : T_DIM, fill); }
    cv_blit(8, y);
}

/* 6.2 soft mute/solo — non-destructive level gate; works while PLAY.
 * LOCK does not block mute/solo (only destructive edits). */
static void tack_apply_mute_solo(void)
{
    uint32_t i;
    uint8_t any_solo = (tack_solo != 0u);
    for (i = 0; i < 4u; i++) {
        uint8_t silenced = (tack_mute & (1u << i)) != 0u;
        if (any_solo && !(tack_solo & (1u << i))) silenced = 1u;
        trk[i].p[P_LEVEL] = silenced ? 0 : tack_level[i];
    }
}

static void tack_set_lane_level(uint8_t lane, int16_t d)
{
    int16_t v;
    lane &= 3u;
    v = (int16_t)tack_level[lane] + d;
    if (v < 0) v = 0;
    if (v > 127) v = 127;
    tack_level[lane] = v;
    tack_level_base[lane] = (uint8_t)v; /* CC7/11 re-scale from this */
    tack_apply_mute_solo();
}

static void tack_set_lane_pan(uint8_t lane, int16_t d)
{
    int16_t v;
    lane &= 3u;
    v = (int16_t)tack_lane_pan[lane] + d;
    if (v < -64) v = -64;
    if (v > 63) v = 63;
    tack_lane_pan[lane] = (int8_t)v;
    trk[lane].p[P_PAN] = (int16_t)v;
}

static void tack_panic(void)
{
    tack_midi_rec_reset();
    tack_crec_mode = 0;
    seq_stop();
    panic_req = (1u << NTRK) - 1u;
    tack_flash("PANIC");
}

/* ---- A/B arrange (phrase × section columns) ---- */
static void tack_phrase_store(uint8_t ab)
{
    uint32_t i;
    ab &= 1u;
    tack_meta_store(tack_phrase_meta[ab]);
    for (i = 0; i < TACK_SLOTS; i++)
        tack_phrase[ab][i] = tack_state.chords[i];
}

static void tack_phrase_load(uint8_t ab)
{
    uint32_t i;
    ab &= 1u;
    for (i = 0; i < TACK_SLOTS; i++)
        tack_state.chords[i] = tack_phrase[ab][i];
    tack_meta_load(tack_phrase_meta[ab]);
    tack_ab_live = ab;
    tack_pending = 1;
}

static void tack_ab_toggle(void)
{
    /* Store live edits into current phrase, swap, load the other. */
    tack_phrase_store(tack_ab_live);
    tack_ab_live ^= 1u;
    tack_phrase_load(tack_ab_live);
    tack_restart();
    tack_flash(tack_ab_live ? "LIVE B" : "LIVE A");
}

static void tack_arr_advance(void)
{
    uint8_t next_ab;
    if (!tack_arr_on || tack_arr_len == 0u) return;
    tack_arr_pos = (uint8_t)((tack_arr_pos + 1u) % tack_arr_len);
    next_ab = tack_arr[tack_arr_pos] & 1u;
    if (next_ab != tack_ab_live) {
        tack_phrase_store(tack_ab_live);
        tack_phrase_load(next_ab);
        tack_flash(next_ab ? "ARR→B" : "ARR→A");
    }
}

/* ---- MIDI CC7/11/FX → TACK masters (document in README) ---- */
static void tack_midi_apply_levels(void)
{
    uint32_t i;
    /* master * expression / 127² scaled onto stored base levels */
    uint16_t scale = (uint16_t)tack_midi_master * (uint16_t)tack_midi_expr;
    for (i = 0; i < 4u; i++) {
        int16_t v = (int16_t)(((uint32_t)tack_level_base[i] * scale) / (127u * 127u));
        if (v > 127) v = 127;
        tack_level[i] = v;
        if (!(tack_mute & (1u << i))) trk[i].p[P_LEVEL] = v;
    }
}

void tack_midi_cc(uint8_t cc, uint8_t val)
{
    uint32_t i;
    val &= 127u;
    /* 6.13: LEARN maps always first — arm + mapped targets (VOL/EXPR/FX/CHAOS/…). */
    tack_learn_apply_cc(cc, val);
    switch (cc) {
    case 7: /* Master level — skip when LEARN mapped VOL to this CC (declutter) */
        if (tack_learn_map[0] == cc) break;
        tack_midi_master = val;
        for (i = 0; i < 4u; i++)
            if (tack_level_base[i] == 0 && tack_level[i])
                tack_level_base[i] = (uint8_t)tack_level[i];
        tack_midi_apply_levels();
        break;
    case 11: /* Expression — skip when LEARN mapped EXPR to this CC */
        if (tack_learn_map[1] == cc) break;
        tack_midi_expr = val ? val : 1u;
        tack_midi_apply_levels();
        break;
    case 91: /* Reverb wet master */
        tack_fx_rev = val;
        tack_apply_fx_amounts();
        break;
    case 93: /* Chorus */
        tack_fx_cho = val;
        tack_apply_fx_amounts();
        if (tack_shimmer) tack_apply_shimmer();
        break;
    case 94: /* Delay */
        tack_fx_dly = val;
        tack_apply_fx_amounts();
        break;
    case 1: /* Mod wheel → shimmer amount 0..3 */
        tack_shimmer = (uint8_t)(val / 43u);
        if (val >= 120u) tack_shimmer = 3u;
        tack_apply_shimmer();
        break;
    default:
        break;
    }
}

static void tack_crec_live_erase(void)
{
    /* While PLAY after REC OK: wipe landing under playhead (HELD or LOOP). Never during REC arm. */
    uint8_t step16, i, j, found = 0xFFu;
    if (tack_crec_mode == 1u) { tack_flash("STOP REC"); return; }
    if (tack_crec_len == 0u || !song.playing) {
        tack_flash("ERASE NEED PLAY");
        return;
    }
    if (!tack_crec_steps_ready) tack_crec_ms_to_steps();
    step16 = tack_seq_step16();
    /* Prefer exact step match; else nearest prior landing. */
    for (i = 0; i < tack_crec_len; i++) {
        if (tack_crec_step[i] == step16) { found = i; break; }
    }
    if (found == 0xFFu) {
        /* erase the entry that last fired (pos-1) */
        if (tack_crec_len) found = (uint8_t)((tack_crec_pos + tack_crec_len - 1u) % tack_crec_len);
    }
    if (found == 0xFFu || found >= tack_crec_len) { tack_flash("NO LAND"); return; }
    for (j = found; j + 1u < tack_crec_len; j++) {
        tack_crec[j] = tack_crec[j + 1u];
        tack_crec_step[j] = tack_crec_step[j + 1u];
        tack_crec_ms[j] = tack_crec_ms[j + 1u];
    }
    tack_crec_len--;
    if (tack_crec_pos >= tack_crec_len && tack_crec_len) tack_crec_pos = 0;
    if (!tack_crec_len) { tack_crec_mode = 0; tack_flash("ERASED EMPTY"); }
    else tack_flash("ERASE LAND");
}

static void tack_flush_bar_queues(void)
{
    if (tack_q_style != 0xFFu) {
        uint8_t s = tack_q_style; tack_q_style = 0xFFu;
        tack_pick_style(s);
    }
    if (tack_q_snd != 0xFFu) {
        uint8_t b = tack_q_snd; tack_q_snd = 0xFFu;
        tack_apply_sound_bank(b);
        tack_flash("SND@BAR");
    }
    if (tack_q_kit != 0xFFu) {
        uint8_t k = tack_q_kit; tack_q_kit = 0xFFu;
        tack_apply_classic_kit(k);
        tack_flash("KIT@BAR");
    }
    if (tack_q_fxsc != 0xFFu) {
        uint8_t sc = tack_q_fxsc; tack_q_fxsc = 0xFFu;
        tack_apply_fx_scene(sc); /* flashes scene name — no FX@BAR junk */
    }
    if (tack_q_perf != 0xFFu) {
        uint8_t i = tack_q_perf; tack_q_perf = 0xFFu;
        tack_perf_recall(i);
        tack_flash("PERF@BAR");
    }
}

static void tack_queue_style(uint8_t s)
{
    s %= TACK_STYLE_COUNT;
    if (song.playing) {
        tack_q_style = s;
        tack_style_cursor = s;
        tack_flash2("Q ", TACK_STYLE_NAME[s]);
    } else {
        tack_pick_style(s);
    }
}

static void tack_queue_snd(uint8_t b)
{
    b %= TACK_LIB_N; /* 10 libs incl. TECHNO+CLASSIC+INDUSTRIAL+JUNO — never clamps Ambient→Kits */
    if (song.playing) { tack_q_snd = b; tack_flash2("Q ", TACK_BANK_NAME[b]); }
    else tack_apply_sound_bank(b);
}

static void tack_queue_kit(uint8_t k)
{
    k %= 4u;
    if (song.playing) { tack_q_kit = k; tack_flash2("Q KIT ", TACK_KIT_TAG[k]); }
    else tack_apply_classic_kit(k);
}

static void tack_queue_fxsc(uint8_t sc)
{
    sc %= 4u;
    if (song.playing) { tack_q_fxsc = sc; tack_flash2("Q FX ", TACK_FXSC_NAME[sc]); }
    else tack_apply_fx_scene(sc);
}

static uint8_t tack_is_locked(void)
{
    /* Show lock: explicit long-OCT+ only. PLAY must leave selection free (5.2). */
    return tack_show_lock ? 1u : 0u;
}

/* 5.7 calm Home: SET strip + soft-key hints only while MORE (SAVE held / 2s after nudge). */
static void tack_nudge_more(void)
{
    tack_more_until = fm1_ms + 2000u;
    if (!tack_more_until) tack_more_until = 1u;
}

static uint8_t tack_home_show_more(void)
{
    if (tack_set_browse || tack_arr_edit) return 1u;
    if (tack_more_until && (int32_t)(tack_more_until - fm1_ms) > 0) return 1u;
    return 0u;
}

static uint8_t tack_kit_index_now(void)
{
    uint8_t i;
    for (i = 0; i < 4u; i++)
        if (trk[3].preset == TACK_KIT_PRESET[i]) return i;
    return 0;
}

static void tack_perf_save(uint8_t i)
{
    TackPerf *p;
    uint32_t n, ab;
    i %= TACK_PERF_N;
    p = &tack_perf[i];
    tack_phrase_store(tack_ab_live); /* sync live edits into phrase bank */
    p->used = 1u;
    p->style = tack_style_idx;
    p->snd_bank = tack_snd_bank;
    p->amb_tex = tack_amb_tex;
    p->fx_scene = tack_fx_scene;
    p->ab_live = tack_ab_live & 1u;
    p->arr_len = tack_arr_len ? tack_arr_len : 4u;
    p->arr_on = tack_arr_on;
    p->dens = tack_dens;
    p->beat_kit = tack_beat_kit;
    p->kit = tack_kit_index_now();
    p->drm_dist = tack_drm_dist;
    p->drm_comp = tack_drm_comp;
    for (n = 0; n < TACK_ARR_MAX; n++) p->arr[n] = tack_arr[n];
    for (n = 0; n < 4u; n++) {
        p->lane_pan[n] = tack_lane_pan[n];
        p->level_base[n] = tack_level_base[n] ? tack_level_base[n] : (uint8_t)tack_level[n];
    }
    for (ab = 0; ab < 2u; ab++)
        for (n = 0; n < TACK_SLOTS; n++)
            p->phrase[ab][n] = tack_phrase[ab][n];
    tack_set_sel = i;
    tack_set_live = i;
    {
        char msg[12];
        msg[0] = 'P'; msg[1] = (char)('1' + i); msg[2] = 0;
        tack_flash2("SAVED ", msg);
    }
}

static void tack_perf_recall(uint8_t i)
{
    TackPerf *p;
    uint32_t n, ab;
    i %= TACK_PERF_N;
    p = &tack_perf[i];
    if (!p->used) {
        char msg[12];
        msg[0] = 'P'; msg[1] = (char)('1' + i); msg[2] = 0;
        tack_flash2(msg, " EMPTY");
        return;
    }
    /* Timbre + arrange snapshot. Does NOT clear REC phrase / crec. */
    tack_pick_style(tack_style_migrate(p->style));
    tack_snd_bank = p->snd_bank % TACK_LIB_N;
    tack_snd_cursor = tack_snd_bank;
    tack_amb_tex = p->amb_tex;
    if (tack_snd_bank == 0u) {
        tack_apply_classic_kit(p->kit);
    } else if (tack_snd_bank >= 1u && tack_snd_bank <= 3u) {
        tack_apply_amb_texture();
    } else {
        /* 303/ACID — re-apply bank (sounds only; style already set above). */
        tack_apply_sound_bank(4);
    }
    tack_apply_fx_scene(p->fx_scene);
    tack_dens = p->dens;
    tack_beat_kit = p->beat_kit;
    tack_drm_dist = p->drm_dist;
    tack_drm_comp = p->drm_comp;
    tack_apply_drum_bus();
    tack_arr_len = p->arr_len ? p->arr_len : 4u;
    tack_arr_on = p->arr_on;
    tack_arr_pos = 0;
    for (n = 0; n < TACK_ARR_MAX; n++) tack_arr[n] = p->arr[n];
    for (ab = 0; ab < 2u; ab++)
        for (n = 0; n < TACK_SLOTS; n++)
            tack_phrase[ab][n] = p->phrase[ab][n];
    tack_phrase_load(p->ab_live & 1u);
    for (n = 0; n < 4u; n++) {
        tack_lane_pan[n] = p->lane_pan[n];
        trk[n].p[P_PAN] = (int16_t)p->lane_pan[n];
        tack_level_base[n] = p->level_base[n];
        tack_level[n] = (int16_t)p->level_base[n];
        if (!(tack_mute & (1u << n))) trk[n].p[P_LEVEL] = tack_level[n];
    }
    tack_midi_apply_levels();
    tack_patterns();
    tack_set_sel = i;
    tack_set_live = i;
    tack_restart();
    {
        char msg[12];
        msg[0] = 'P'; msg[1] = (char)('1' + i); msg[2] = 0;
        tack_flash2("LIVE ", msg);
    }
}

static void tack_queue_perf(uint8_t i)
{
    i %= TACK_PERF_N;
    tack_set_sel = i;
    if (song.playing) {
        tack_q_perf = i;
        {
            char msg[12];
            msg[0] = 'P'; msg[1] = (char)('1' + i); msg[2] = 0;
            tack_flash2("Q ", msg);
        }
    } else {
        tack_perf_recall(i);
    }
}

/* Factory starter performances — Style+Sounds+FX+A/B defaults for cold start.
 * Writes TackPerf only; never clears hand REC / crec. User long-SAVE still overwrites. */
static void tack_perf_write_starter(uint8_t i, uint8_t style, uint8_t snd_bank,
                                    uint8_t amb_tex, uint8_t fx_scene, uint8_t dens,
                                    uint8_t kit, uint8_t drm_dist, uint8_t drm_comp)
{
    TackPerf *p;
    uint32_t n, ab;
    i %= TACK_PERF_N;
    p = &tack_perf[i];
    p->used = 1u;
    p->style = style % TACK_STYLE_COUNT;
    p->snd_bank = snd_bank % TACK_LIB_N;
    p->amb_tex = amb_tex;
    p->fx_scene = fx_scene & 3u;
    p->ab_live = 0;
    p->arr_len = tack_arr_len ? tack_arr_len : 4u;
    p->arr_on = 1u;
    p->dens = dens > 100u ? 100u : dens;
    p->kit = kit % 4u;
    p->beat_kit = (p->kit >= 2u) ? (uint8_t)(p->kit - 1u) : 0u;
    p->drm_dist = drm_dist;
    p->drm_comp = drm_comp;
    for (n = 0; n < TACK_ARR_MAX; n++) p->arr[n] = tack_arr[n];
    for (n = 0; n < 4u; n++) {
        p->lane_pan[n] = tack_lane_pan[n];
        p->level_base[n] = tack_level_base[n] ? tack_level_base[n] : (uint8_t)tack_level[n];
    }
    /* Snapshot current A/B phrases — starters share boot harmony; style on recall drives feel. */
    for (ab = 0; ab < 2u; ab++)
        for (n = 0; n < TACK_SLOTS; n++)
            p->phrase[ab][n] = tack_phrase[ab][n];
}

static void tack_perf_seed_starters(void)
{
    /* P1 Beatles · P2 Jazz · P3 Jungle · P4 Reggae · P5 Funk · P6 Bossa · P7 Breaks · P8 Ambient.
     * dens/dist/comp match style tables. FX: Room/Dub/Dry/Shimmer. Ambient uses AMBIENT bank. */
    tack_perf_write_starter(0, TACK_STYLE_BEATLES, 0, 0, 1, 22, 0, 14, 36); /* ROOM */
    tack_perf_write_starter(1, TACK_STYLE_JAZZ,    0, 0, 1, 44, 0,  8, 40); /* ROOM */
    tack_perf_write_starter(2, TACK_STYLE_JUNGLE,  0, 0, 0, 34, 0, 22, 56); /* DRY */
    tack_perf_write_starter(3, TACK_STYLE_REGGAE,  0, 0, 2, 38, 0, 10, 42); /* DUB */
    tack_perf_write_starter(4, TACK_STYLE_FUNK,    0, 0, 1, 24, 0, 10, 50); /* ROOM */
    tack_perf_write_starter(5, TACK_STYLE_BOSSA,   0, 0, 1, 28, 0,  6, 38); /* ROOM */
    tack_perf_write_starter(6, TACK_STYLE_BREAKS,  0, 0, 0, 30, 0, 18, 64); /* DRY */
    tack_perf_write_starter(7, TACK_STYLE_AMBIENT, 1, 0, 3, 10, 0,  4, 18); /* SHIMMER + AMB bank */
    tack_set_sel = 0;
    tack_set_live = 0;
}

/* Chaos: only when enabled — mild dens/hat mutate each bar wrap. */
/* AUTO: TACK steers harmony — each bar queues a fresh move into the next
 * slot (same as repeated K2 GO), rotating the pick so it never stagnates.
 * HOLD freezes sounding chords so Auto stays quiet while held.
 * Cycle first chord: HOME (tonic) unless user already chose a start chord. */
static TackChord tack_make_home(void)
{
    const TackModeInfo *m = tack_mode_info(tack_state.mode);
    TackChord c;
    c.root = tack_mod12((int)tack_state.root); /* degree 0 = key tonic = HOME */
    c.quality = (m && m->count) ? m->qualities[0] : TACK_Q_MAJ;
    return c;
}

static void tack_auto_begin_cycle(void)
{
    /* Prefer last manual pick/capture; otherwise force slot 0 = HOME. */
    if (tack_user_chose)
        tack_state.chords[0] = tack_user_chord;
    else
        tack_state.chords[0] = tack_make_home();
    tack_state.focus = 0;
    tack_state.current = 0;
    tack_pick_slot = 0; /* 6.2: first key hits current/first chord, then advances */
    tack_sel = 0;
    tack_loop = 0;
    tack_restart();
}

static void tack_auto_set(uint8_t on)
{
    tack_auto = on ? 1u : 0u;
    if (tack_auto) {
        tack_auto_begin_cycle();
        tack_chord_grid = tack_auto_next_grid();
        tack_chord_grid_armed = 1u;
        if (!song.playing) {
            seq_start();
            tack_flash(tack_user_chose ? "AUTO PLAY" : "AUTO HOME");
        } else {
            tack_flash(tack_user_chose ? "AUTO ON" : "AUTO HOME");
        }
    } else {
        tack_chord_grid_armed = 0;
        tack_flash("AUTO OFF");
    }
}

static void tack_auto_steer(uint8_t bar_now)
{
    TackChord mv[TACK_MOVES];
    TackChord cur_next;
    uint8_t next, pick, t, changed = 0u;
    if (!tack_auto || tack_hold || tack_crec_mode) return;
    tack_state.focus = bar_now;
    tack_next_moves(&tack_state, mv);
    next = (uint8_t)((bar_now + 1u) & 3u);
    cur_next = tack_state.chords[next];
    /* Walk picks so Auto keeps choosing a different next chord. */
    pick = (uint8_t)((tack_sel + 1u + (tack_loop & 3u) + bar_now) % TACK_MOVES);
    /* Beatles: favor I–vi–IV–V / I–V–vi–IV movement energy among next moves. */
    if (tack_style_idx == TACK_STYLE_BEATLES) {
        static const uint8_t pref[4] = {0u, 9u, 5u, 7u}; /* I, vi, IV, V semitone offs */
        uint8_t best = 0xFF, bi = 0;
        for (t = 0u; t < TACK_MOVES; t++) {
            uint8_t p = (uint8_t)((pick + t) % TACK_MOVES);
            uint8_t off = tack_mod12((int)mv[p].root - (int)tack_state.root);
            uint8_t sc = 4u, k; /* 4 = outside I/vi/IV/V — V (3) still wins */
            if (mv[p].root == cur_next.root && mv[p].quality == cur_next.quality) continue;
            for (k = 0u; k < 4u; k++) if (off == pref[k]) { sc = (uint8_t)k; break; }
            if (sc < best) { best = sc; bi = p; changed = 1u; }
        }
        if (changed) pick = bi;
    }
    if (!changed) for (t = 0u; t < TACK_MOVES; t++) {
        uint8_t p = (uint8_t)((pick + t) % TACK_MOVES);
        if (mv[p].root != cur_next.root || mv[p].quality != cur_next.quality) {
            pick = p;
            changed = 1u;
            break;
        }
    }
    if (!changed) {
        tack_sel = pick;
        return;
    }
    tack_push_undo();
    tack_state.chords[next] = mv[pick];
    tack_sel = pick;
    tack_pending = 1;
    /* Audible (patterns rebuild) + visible toast; soft redraw avoids flicker. */
    tack_flash2("AUTO ", TACK_NOTES[mv[pick].root % 12u]);
}

static void tack_air_morph_tick(void)
{
    int16_t cur, tgt, step;
    if (tack_air_freeze || tack_hold || !tack_morph_left) return;
    cur = song.g[G_CDEPTH];
    tgt = (int16_t)tack_air_target;
    step = (int16_t)((tgt - cur) / (int16_t)tack_morph_left);
    if (step == 0) step = (tgt > cur) ? 1 : (tgt < cur ? -1 : 0);
    song.g[G_CDEPTH] = (int16_t)(cur + step);
    tack_morph_left--;
    if (!tack_morph_left) song.g[G_CDEPTH] = tgt;
}

static void tack_chaos_tick(void)
{
    if (!tack_chaos) return;
    uint8_t h = tack_drum_hash(tack_last_bar, 0, 99);
    int16_t d = (int16_t)tack_dens + (int16_t)((h & 15u) - 7);
    if (d < 8) d = 8;
    if (d > 90) d = 90;
    tack_dens = (uint8_t)d;
    if ((h & 0x30u) == 0x30u) tack_roll_take();
}

void tack_app_input(void)
{
    tack_midi_rec_drain();
    int32_t k1 = panel_enc(EN_K1);
    int32_t k2 = panel_enc(EN_K2);
    int32_t k3 = panel_enc(EN_K3);
    int32_t k4 = panel_enc(EN_K4);
    int32_t sel = panel_enc(EN_SELECT);
    int32_t algo = panel_enc(EN_ALGO);
    int32_t preset = panel_enc(EN_PRESET);
    uint32_t released = 0;
    uint32_t pressed = fm1_input_edges(&released);
    uint32_t held_notes = fm1_in.notes;
    uint8_t pg = tack_page;

    /* 6.16/6.17 boot splash: skip on edge while showing; expire by time so MORE/Zen work.
     * tack_boot_until is an absolute fm1_ms deadline — bare nonzero must NOT swallow forever. */
    if (tack_boot_until) {
        if ((int32_t)(fm1_ms - tack_boot_until) >= 0) {
            tack_boot_until = 0; /* expired — fall through; do not eat this frame */
        } else {
            if (k1 || k2 || k3 || k4 || sel || algo || preset || pressed || released) {
                tack_boot_until = 0;
                tack_drawn = 0; /* Home full-draws once */
            }
            return; /* only while splash still showing */
        }
    }

    /* SELECT: on STYLE = confirm style+BPM; else fine BPM on INT (USB/TRS = clock follow). */
    if (sel) {
        if (pg == TACK_PG_STYLE) {
            if (tack_is_locked()) tack_flash("LOCKED");
            else tack_queue_style(tack_style_cursor); /* apply style BPM — not a fine nudge */
        } else if (!song.g[G_CLOCK]) {
            song.g[G_BPM] = (int16_t)clamp((int32_t)song.g[G_BPM] + sel * (sel > 1 || sel < -1 ? 2 : 1), 40, 140);
            tack_flash_bpm();
        } else {
            tack_flash(song.g[G_CLOCK] == 1 ? "CLK USB" : "CLK TRS");
        }
    }
    /* ALGO = engine; PRESET = library presets on focused lane (6.3). */
    if (algo) tack_cycle_engine(algo > 0 ? 1 : -1);
    if (preset) tack_cycle_preset(preset > 0 ? 1 : -1);

    if (held_notes != tack_last_notes) {
        uint8_t n[4], cnt = 0;
        uint32_t bits = held_notes;
        uint32_t rising = held_notes & ~tack_last_notes;
        uint8_t did_pick = 0;
        tack_last_notes = held_notes;
        /* HOME + LOOP: physical keys 1–6 = moves[0..5] (genre-aware via style_rebind).
         * 6.28-rc1: KEEP this during REC — do not record keys 1-6 as pitches.
         * Played-note recording is USB/TRS MIDI via tack_midi_note().
         * Single-key press applies to pick slot then advances. Multi-key
         * still builds a custom chord (3+). Live-select while PLAY kept. */
        if ((pg == TACK_PG_HOME || pg == TACK_PG_LOOP) && rising && (rising & (rising - 1u)) == 0u
            && __builtin_popcount(held_notes) == 1) {
            uint8_t bit = (uint8_t)__builtin_ctz(rising);
            if (bit < (uint8_t)TACK_MOVES) {
                tack_pick_apply(bit);
                did_pick = 1;
            }
        }
        if (!did_pick) {
            bits = held_notes;
            while (bits && cnt < 4u) {
                uint8_t b = (uint8_t)__builtin_ctz(bits);
                n[cnt++] = (uint8_t)(53u + b);
                bits &= bits - 1u;
            }
            if (cnt >= 3u) {
                uint8_t bar_now = tack_seq_bar();
                tack_push_undo();
                tack_bar_lit_n[bar_now] = 0;
                tack_state.chords[bar_now].root = (uint8_t)(n[0] % 12u);
                tack_state.chords[bar_now].quality = tack_kb_quality(n, cnt);
                tack_slot_owned[bar_now] = 1u;
                tack_user_chord = tack_state.chords[bar_now];
                tack_user_chose = 1u;
                tack_state.focus = bar_now;
                tack_pick_slot = bar_now;
                if (tack_crec_mode == 1u) {
                    /* Free-capture only — no rebind / no seq APPLY / no restart. */
                    tack_crec_capture(&tack_state.chords[bar_now]);
                } else {
                    tack_style_rebind(); /* mode/style opt-in path outside REC */
                    if (tack_crec_mode == 2u && tack_crec_len > 0u) {
                        uint8_t i = (uint8_t)(tack_crec_pos % tack_crec_len);
                        if (i > 0) i = (uint8_t)(i - 1u);
                        tack_crec_store_token_meta(i);
                        tack_crec[i] = tack_state.chords[bar_now];
                        tack_crec_ms[i] = (fm1_ms >= tack_crec_loop_t0) ? (fm1_ms - tack_crec_loop_t0) : 0u;
                        tack_crec_steps_ready = 0;
                    }
                    tack_phrase_store(tack_ab_live);
                    tack_restart();
                }
            }
        }
    }

    /* HOME: back from any edit page; short on home = next lane; long = arr-edit. */
    if (pressed & (1u << panel.btn[B_HOME])) {
        if (pg != TACK_PG_HOME) {
            tack_go_home();
            pg = tack_page;
        } else {
            tack_home_down_ms = fm1_ms ? fm1_ms : 1u;
            tack_home_long_done = 0;
        }
    }
    if (pg == TACK_PG_HOME && tack_home_down_ms && !tack_home_long_done
        && (fm1_in.buttons & (1u << panel.btn[B_HOME]))
        && (uint32_t)(fm1_ms - tack_home_down_ms) >= 450u) {
        tack_home_long_done = 1;
        if (tack_is_locked()) {
            tack_flash("LOCKED");
        } else {
            tack_arr_edit ^= 1u;
            if (tack_arr_edit) tack_flash("ARR EDIT");
            else {
                tack_arr_on = 1u;
                tack_flash(tack_ab_live ? "ARR LIVE B" : "ARR LIVE A");
            }
        }
    }
    if (released & (1u << panel.btn[B_HOME])) {
        if (pg == TACK_PG_HOME && tack_home_down_ms && !tack_home_long_done) {
            tack_mute_sel = (uint8_t)((tack_mute_sel + 1u) & 3u);
            tack_flash2("LANE ", TACK_LANE[tack_mute_sel]);
        }
        tack_home_down_ms = 0;
        pg = tack_page;
    }

    /* Open edit pages from home (or jump between). Show lock: ignore while locked.
     * ENV is deferred: short = GEN tab (toggle); long ≥450ms = FEEL. */
    if (pressed & ((1u << panel.btn[B_EDIT]) | (1u << panel.btn[B_SCL]) | (1u << panel.btn[B_LFO])
                 | (1u << panel.btn[B_FX]) | (1u << panel.btn[B_GLO]))) {
        if (tack_is_locked()) {
            /* Allow HOME return if already on an edit page; else flash LOCKED. */
            if (pg != TACK_PG_HOME) tack_go_home();
            else tack_flash("LOCKED");
            pg = tack_page;
        } else {
            if (pressed & (1u << panel.btn[B_EDIT])) {
                if (pg == TACK_PG_SOUNDS) tack_go_home();
                else tack_open_page(TACK_PG_SOUNDS);
                pg = tack_page;
            }
            if (pressed & (1u << panel.btn[B_SCL])) {
                if (pg == TACK_PG_STYLE) tack_go_home();
                else tack_open_page(TACK_PG_STYLE);
                pg = tack_page;
            }
            if (pressed & (1u << panel.btn[B_LFO])) {
                if (pg == TACK_PG_ALGO) tack_go_home();
                else tack_open_page(TACK_PG_ALGO);
                pg = tack_page;
            }
            if (pressed & (1u << panel.btn[B_FX])) {
                if (pg == TACK_PG_FX) tack_go_home();
                else tack_open_page(TACK_PG_FX);
                pg = tack_page;
            }
            if (pressed & (1u << panel.btn[B_GLO])) {
                /* 6.10: GLO is navigation only — never remake / never patterns.
                 * Off-drums → PART. On PART → FEEL. On FEEL → HOME. */
                if (pg == TACK_PG_DRUMS) {
                    if (tack_drm_depth == 0u) {
                        tack_drm_depth = 1u;
                        tack_flash("FEEL");
                    } else {
                        tack_go_home();
                    }
                } else {
                    /* GLO → PART: nav only — claim live hits, never remake/patterns. */
                    tack_open_page(TACK_PG_DRUMS);
                    tack_drm_depth = 0u;
                    tack_drm_part_own = 1;
                }
                pg = tack_page;
            }
        }
    }
    /* ENV short → GEN tab (toggle, same parity as EDIT→Sounds); long → FEEL. Under LOCK: flash only. */
    if (pressed & (1u << panel.btn[B_ENV])) {
        if (tack_is_locked()) {
            if (pg != TACK_PG_HOME) tack_go_home();
            else tack_flash("LOCKED");
            pg = tack_page;
            tack_env_down_ms = 0; tack_env_long_done = 1;
        } else {
            tack_env_down_ms = fm1_ms ? fm1_ms : 1u;
            tack_env_long_done = 0;
        }
    }
    if (tack_env_down_ms && !tack_env_long_done
        && (fm1_in.buttons & (1u << panel.btn[B_ENV]))
        && (uint32_t)(fm1_ms - tack_env_down_ms) >= 450u) {
        /* Long ENV → FEEL (secondary). */
        tack_env_long_done = 1;
        if (pg == TACK_PG_FEEL) tack_go_home();
        else tack_open_page(TACK_PG_FEEL);
        pg = tack_page;
    }
    if (released & (1u << panel.btn[B_ENV])) {
        /* Short ENV → GEN tab toggle. */
        if (tack_env_down_ms && !tack_env_long_done) {
            if (pg == TACK_PG_GEN) tack_go_home();
            else tack_open_page(TACK_PG_GEN);
            pg = tack_page;
        }
        tack_env_down_ms = 0;
    }

    /* REC short → LOOP tab from any page except FEEL (HOLD) and LOOP (crec). Same nav parity as EDIT. */
    if (pg != TACK_PG_FEEL && pg != TACK_PG_LOOP) {
        if (pressed & (1u << panel.btn[B_REC])) {
            if (tack_is_locked()) {
                if (pg != TACK_PG_HOME) tack_go_home();
                else tack_flash("LOCKED");
                pg = tack_page;
            } else {
                tack_open_page(TACK_PG_LOOP);
                tack_style_rebind();
                pg = tack_page;
            }
        }
    }

    /* Octave up/down — physical OCT±; harmonic lanes only (comp/bass/air). Range −2..+2.
     * On HOME: OCT+ is deferred (short=octave, long=show LOCK) so lock does not bump pitch. */
    {
        uint32_t oct_up = (1u << panel.btn[B_OCTUP]);
        uint32_t oct_dn = (1u << panel.btn[B_OCTDN]);
        if ((pressed & oct_up) && (pressed & oct_dn)) {
            tack_set_octave(0); /* both: reset */
            tack_k4_lock_down_ms = 0; tack_k4_lock_done = 1; /* cancel pending lock */
        } else if (pressed & oct_dn) {
            tack_set_octave((int8_t)(tack_octave - 1));
        } else if ((pressed & oct_up) && pg != TACK_PG_HOME) {
            tack_set_octave((int8_t)(tack_octave + 1));
        }
        /* HOME OCT+ armed in page-local below. */
    }

    /* -------- page-local controls -------- */
    if (pg == TACK_PG_HOME) {
        if (tack_arr_edit) {
            /* Spare arrange strip edit — NOT stock 8×8. K1 col, K2 A↔B, K3 len, K4 on/off. */
            if (tack_is_locked() && (k1 || k2 || k3 || k4)) {
                tack_flash("LOCKED");
            } else if (!tack_is_locked() && k1) {
                int32_t c = (int32_t)tack_arr_sel + (k1 > 0 ? 1 : -1);
                if (c < 0) c = (int32_t)tack_arr_len - 1;
                if (c >= (int32_t)tack_arr_len) c = 0;
                tack_arr_sel = (uint8_t)c;
                tack_flash("ARR COL");
            }
            if (!tack_is_locked() && k2 && (uint32_t)(fm1_ms - tack_k2_ms) > 180u) {
                tack_k2_ms = fm1_ms;
                tack_arr[tack_arr_sel % TACK_ARR_MAX] ^= 1u;
                tack_flash(tack_arr[tack_arr_sel] ? "SEC B" : "SEC A");
            }
            if (!tack_is_locked() && k3 && (uint32_t)(fm1_ms - tack_k3_ms) > 250u) {
                tack_k3_ms = fm1_ms;
                tack_arr_len = (tack_arr_len == 4u) ? 8u : 4u;
                if (tack_arr_sel >= tack_arr_len) tack_arr_sel = 0;
                tack_flash(tack_arr_len == 8u ? "ARR 8" : "ARR 4");
            }
            if (!tack_is_locked() && k4 && (uint32_t)(fm1_ms - tack_k4_ms) > 250u) {
                tack_k4_ms = fm1_ms;
                tack_arr_on ^= 1u;
                tack_flash(tack_arr_on ? "ARR ON" : "ARR OFF");
            }
        } else {
            if (k1) {
                if (tack_set_browse) {
                    /* SAVE held: browse setlist P1..P8 without firing. */
                    int32_t s = (int32_t)tack_set_sel + (k1 > 0 ? 1 : -1);
                    if (s < 0) s = (int32_t)TACK_PERF_N - 1;
                    if (s >= (int32_t)TACK_PERF_N) s = 0;
                    tack_set_sel = (uint8_t)s;
                    {
                        char msg[8];
                        msg[0] = 'P'; msg[1] = (char)('1' + tack_set_sel); msg[2] = 0;
                        tack_flash(msg);
                    }
                } else {
                    tack_sel = (uint8_t)((tack_sel + (k1 > 0 ? 1 : 5)) % 6u);
                    tack_flash("K1 MOVE");
                }
            }
            if (k2 && (uint32_t)(fm1_ms - tack_k2_ms) > 250u) {
                tack_k2_ms = fm1_ms;
                if (tack_home_show_more()) {
                    /* 6.15 MORE: K2 → LEARN even while SAVE held (set_browse); K1 still browses P1..P8. */
                    tack_open_page(TACK_PG_LEARN);
                    tack_flash("LEARN");
                } else {
                    tack_commit(); /* pick_apply: undo + apply sel + advance + toast */
                    tack_phrase_store(tack_ab_live); /* keep live phrase in sync */
                }
            }
            /* K3 = live A↔B (phrase) — stays usable under show lock. */
            if (k3) {
                if ((uint32_t)(fm1_ms - tack_k3_ms) > 280u) {
                    tack_k3_ms = fm1_ms;
                    tack_ab_toggle();
                }
            }
            /* K4 = mode (LOCK is long OCT+). MORE: K4 → Zen. */
            if (k4 && (uint32_t)(fm1_ms - tack_k4_ms) > 300u) {
                tack_k4_ms = fm1_ms;
                if (tack_home_show_more()) {
                    /* 6.15 MORE: K4 → Zen while SAVE held too (set_browse no longer blocks). */
                    tack_zen_enter();
                } else {
                    if (k4 > 0) tack_next_mode();
                    else {
                        tack_mode_idx = (uint8_t)((tack_mode_idx + TACK_MODE_COUNT - 1u) % TACK_MODE_COUNT);
                        tack_set_mode_keep(&tack_state, tack_mode_idx, tack_slot_owned);
                        tack_restart();
                    }
                    tack_flash("K4 MODE");
                }
            }
        }
        if (pressed & (1u << panel.btn[B_SEQ])) { tack_commit(); tack_phrase_store(tack_ab_live); }
        /* 6.8 ARP button: short = open Drums PART (release); long ≥450ms = BRIDGE.
         * Mute lives on FEEL / long-ARP off-Home. Style/lib/kit untouched. */
        if (pressed & (1u << panel.btn[B_ARP])) {
            tack_k3_down_ms = fm1_ms ? fm1_ms : 1u;
            tack_k3_long_done = 0;
        }
        if (tack_k3_down_ms && !tack_k3_long_done
            && (fm1_in.buttons & (1u << panel.btn[B_ARP]))
            && (uint32_t)(fm1_ms - tack_k3_down_ms) >= 450u) {
            if (tack_is_locked()) {
                tack_k3_long_done = 1;
                tack_flash("LOCKED");
            } else {
                tack_k3_long_done = 1;
                tack_push_undo();
                tack_bridge();
                tack_phrase_store(tack_ab_live);
                tack_flash("BRIDGE");
            }
        }
        /* SAVE: short = queue setlist slot @ bar-end; long = save Performance into slot.
         * Hold SAVE + K1 = browse P1..P8. Live erase still via REC path when crec PLAY. */
        if (pressed & (1u << panel.btn[B_SAVE])) {
            tack_save_down_ms = fm1_ms ? fm1_ms : 1u;
            tack_save_long_done = 0;
            tack_set_browse = 1u;
            tack_nudge_more(); /* MORE: show SET + hints ~2s */
        }
        /* 6.15: hold SAVE keeps MORE chrome alive (Zen/LEARN stay reachable mid-hold). */
        if (fm1_in.buttons & (1u << panel.btn[B_SAVE]))
            tack_nudge_more();
        if (tack_save_down_ms && !tack_save_long_done
            && (fm1_in.buttons & (1u << panel.btn[B_SAVE]))
            && (uint32_t)(fm1_ms - tack_save_down_ms) >= 550u) {
            tack_save_long_done = 1;
            tack_set_browse = 0;
            if (tack_crec_mode != 1u && tack_crec_len && song.playing && !tack_is_locked())
                tack_crec_live_erase();
            else if (tack_is_locked())
                tack_flash("LOCKED"); /* no accidental overwrite under LOCK */
            else
                tack_perf_save(tack_set_sel);
        }
        if (released & (1u << panel.btn[B_SAVE])) {
            if (!tack_save_long_done) {
                if (tack_crec_mode != 1u && tack_crec_len && song.playing && !tack_is_locked())
                    tack_crec_live_erase();
                else {
                    tack_queue_perf(tack_set_sel); /* bar-end while PLAY */
                    tack_nudge_more();
                }
            }
            tack_save_down_ms = 0;
            tack_set_browse = 0;
        }
        /* HOME OCT+: short = octave up; long ≥500ms = show LOCK toggle. */
        if (pressed & (1u << panel.btn[B_OCTUP])) {
            if (!(fm1_in.buttons & (1u << panel.btn[B_OCTDN]))) {
                tack_k4_lock_down_ms = fm1_ms ? fm1_ms : 1u;
                tack_k4_lock_done = 0;
            }
        }
        if (tack_k4_lock_down_ms && !tack_k4_lock_done
            && (fm1_in.buttons & (1u << panel.btn[B_OCTUP]))
            && !(fm1_in.buttons & (1u << panel.btn[B_OCTDN]))
            && (uint32_t)(fm1_ms - tack_k4_lock_down_ms) >= 500u) {
            tack_k4_lock_done = 1;
            tack_show_lock ^= 1u;
            tack_nudge_more();
            tack_flash(tack_show_lock ? "LOCK ON" : "LOCK OFF");
        }
        if (released & (1u << panel.btn[B_OCTUP])) {
            if (!tack_k4_lock_done && tack_k4_lock_down_ms)
                tack_set_octave((int8_t)(tack_octave + 1));
            tack_k4_lock_down_ms = 0;
        }
        /* REC tab handled globally (opens LOOP). Feel REC = HOLD. */
    } else if (pg == TACK_PG_SOUNDS) {
        /* 6.3 clean EDIT: (1) pick LIBRARY → apply + reset presets
         * (2) PRESETS of that library only (K1 browse / K2 apply band / K3 track)
         * MODE and KIT never change here. */
        if (tack_is_locked() && (k1 || k2 || k3 || k4 || (pressed & (1u << panel.btn[B_SAVE])))) {
            tack_flash("LOCKED");
        } else if (!tack_is_locked()) {
            if (tack_snd_depth == 0u) {
                if (k1 && (uint32_t)(fm1_ms - tack_k1_ms) > 160u) {
                    int32_t b = (int32_t)tack_snd_cursor + (k1 > 0 ? 1 : -1);
                    tack_k1_ms = fm1_ms;
                    if (b < 0) b = (int32_t)TACK_LIB_N - 1;
                    if (b >= (int32_t)TACK_LIB_N) b = 0;
                    tack_snd_cursor = (uint8_t)b;
                    tack_flash(TACK_BANK_NAME[tack_snd_cursor]);
                }
                if ((k2 && (uint32_t)(fm1_ms - tack_k2_ms) > 200u)
                    || (pressed & (1u << panel.btn[B_SEQ]))) {
                    if (k2) tack_k2_ms = fm1_ms;
                    /* Library change: presets reset to first of THIS library, then apply. */
                    tack_snd_bank = tack_snd_cursor % TACK_LIB_N;
                    tack_snd_pick = 0;
                    tack_amb_tex = 0;
                    tack_trk_lib_pick[0] = tack_trk_lib_pick[1] = tack_trk_lib_pick[2] = tack_trk_lib_pick[3] = 0;
                    tack_queue_snd(tack_snd_bank);
                    tack_snd_depth = 1;
                    tack_flash(TACK_BANK_NAME[tack_snd_bank]);
                }
            } else {
                /* Presets for current library only */
                uint8_t n = tack_lib_sound_n(tack_snd_bank);
                if (k1 && (uint32_t)(fm1_ms - tack_k1_ms) > 160u) {
                    int32_t p = (int32_t)tack_snd_pick + (k1 > 0 ? 1 : -1);
                    tack_k1_ms = fm1_ms;
                    if (p < 0) p = (int32_t)n - 1;
                    if (p >= (int32_t)n) p = 0;
                    tack_snd_pick = (uint8_t)p;
                    /* 6.4: browse applies to focused line — name matches catalog */
                    tack_apply_lib_sound_track(tack_mute_sel & 3u, tack_snd_bank, tack_snd_pick);
                }
                if (k2 && (uint32_t)(fm1_ms - tack_k2_ms) > 200u) {
                    tack_k2_ms = fm1_ms;
                    tack_apply_edit_sound(tack_snd_bank, tack_snd_pick);
                }
                if (k4 && (uint32_t)(fm1_ms - tack_k4_ms) > 200u) {
                    tack_k4_ms = fm1_ms;
                    tack_snd_depth = 0;
                    tack_snd_cursor = tack_snd_bank % TACK_LIB_N;
                    tack_flash("LIBRARY");
                }
                if (pressed & (1u << panel.btn[B_SEQ]))
                    tack_apply_edit_sound(tack_snd_bank, tack_snd_pick);
            }
            if (k3) {
                /* 6.4: focus line/track; show THAT track's catalog name (do not stamp global pick).
                 * Apply to the focused track via PRESETS / LFO / EN_PRESET. K2 = band. */
                tack_mute_sel = (uint8_t)((tack_mute_sel + (k3 > 0 ? 1 : 3)) & 3u);
                tack_snd_pick = tack_trk_lib_pick[tack_mute_sel & 3u];
                tack_flash2("TRK ", tack_trk_lib_name(tack_mute_sel & 3u));
            }
        }
        if (tack_is_locked() && (pressed & (1u << panel.btn[B_SEQ]))) tack_go_home();
        /* SAVE → PRESETS page = same library list, per-track apply (not engine factory). */
        if (!tack_is_locked() && (pressed & (1u << panel.btn[B_SAVE]))) tack_open_page(TACK_PG_PRESET);
    } else if (pg == TACK_PG_STYLE) {
        if (tack_is_locked() && (k1 || k2 || k3 || k4)) {
            tack_flash("LOCKED");
        } else
        if (!tack_is_locked() && k1 && (uint32_t)(fm1_ms - tack_k1_ms) > 160u) {
            int32_t s = (int32_t)tack_style_cursor + (k1 > 0 ? 1 : -1);
            tack_k1_ms = fm1_ms;
            if (s < 0) s = TACK_STYLE_COUNT - 1;
            if (s >= TACK_STYLE_COUNT) s = 0;
            tack_style_cursor = (uint8_t)s;
            /* browse only — tempo/style apply on SELECT / K2 / SEQ */
            tack_flash(TACK_STYLE_NAME[tack_style_cursor]);
        }
        if (!tack_is_locked() && k2 && (uint32_t)(fm1_ms - tack_k2_ms) > 200u) {
            tack_k2_ms = fm1_ms;
            tack_queue_style(tack_style_cursor);
            if (!song.playing) tack_go_home();
        }
        if (!tack_is_locked() && k3) {
            int16_t d = (int16_t)tack_dens + (k3 > 0 ? 4 : -4);
            int dir = k3 > 0 ? 1 : -1;
            if (d < 0) d = 0;
            if (d > 100) d = 100;
            if ((uint8_t)d != tack_dens) {
                tack_dens = (uint8_t)d;
                if (tack_drm_part_own) tack_drm_density_nudge(dir);
                else tack_patterns();
            }
            tack_flash("DENS");
        }
        if (pressed & (1u << panel.btn[B_SEQ])) {
            if (tack_is_locked()) tack_go_home();
            else { tack_queue_style(tack_style_cursor); if (!song.playing) tack_go_home(); }
        }
    } else if (pg == TACK_PG_ALGO) {
        /* 6.3 LFO = Track → Sound (active library presets only). */
        if (tack_is_locked() && (k1 || k2 || k3 || k4 || (pressed & (1u << panel.btn[B_SAVE])))) {
            tack_flash("LOCKED");
        } else if (!tack_is_locked()) {
            if (k1) {
                tack_mute_sel = (uint8_t)((tack_mute_sel + (k1 > 0 ? 1 : 3)) & 3u);
                tack_snd_pick = tack_trk_lib_pick[tack_mute_sel & 3u]; /* 6.4: show this lane's catalog */
                tack_flash2("TRK ", TACK_LANE[tack_mute_sel & 3u]);
            }
            if ((k2 || k3) && (uint32_t)(fm1_ms - tack_k2_ms) > 140u) {
                int32_t dir = (k2 ? k2 : k3) > 0 ? 1 : -1;
                uint8_t n = tack_lib_sound_n(tack_snd_bank);
                int32_t p = (int32_t)tack_trk_lib_pick[tack_mute_sel & 3u] + dir;
                tack_k2_ms = fm1_ms;
                if (p < 0) p = (int32_t)n - 1;
                if (p >= (int32_t)n) p = 0;
                tack_snd_pick = (uint8_t)p;
                tack_apply_lib_sound_track(tack_mute_sel & 3u, tack_snd_bank, tack_snd_pick);
            }
            if (k4 && (uint32_t)(fm1_ms - tack_k4_ms) > 200u) {
                tack_k4_ms = fm1_ms;
                tack_open_page(TACK_PG_PRESET); /* library presets per track */
            }
            if (pressed & (1u << panel.btn[B_SAVE])) tack_open_page(TACK_PG_PRESET);
        }
        if (pressed & (1u << panel.btn[B_SEQ])) tack_go_home();
    } else if (pg == TACK_PG_PRESET) {
        /* 6.3: PRESETS = active library only (per track). No engine-factory bank. */
        if (tack_is_locked() && (k1 || k2 || k3 || k4 || (pressed & (1u << panel.btn[B_LFO])))) {
            tack_flash("LOCKED");
        } else if (!tack_is_locked()) {
            if (k1) {
                tack_mute_sel = (uint8_t)((tack_mute_sel + (k1 > 0 ? 1 : 3)) & 3u);
                tack_snd_pick = tack_trk_lib_pick[tack_mute_sel & 3u];
                tack_flash2("TRK ", TACK_LANE[tack_mute_sel]);
            }
            if ((k2 || k3) && (uint32_t)(fm1_ms - tack_k2_ms) > 140u) {
                int32_t dir = (k2 ? k2 : k3) > 0 ? 1 : -1;
                uint8_t n = tack_lib_sound_n(tack_snd_bank);
                int32_t p = (int32_t)tack_trk_lib_pick[tack_mute_sel & 3u] + dir;
                tack_k2_ms = fm1_ms;
                if (p < 0) p = (int32_t)n - 1;
                if (p >= (int32_t)n) p = 0;
                tack_snd_pick = (uint8_t)p;
                tack_apply_lib_sound_track(tack_mute_sel & 3u, tack_snd_bank, tack_snd_pick);
            }
            if (k4 && (uint32_t)(fm1_ms - tack_k4_ms) > 200u) {
                tack_k4_ms = fm1_ms;
                tack_open_page(TACK_PG_SOUNDS);
                tack_snd_depth = 0;
                tack_flash("LIBRARY");
            }
            if (pressed & (1u << panel.btn[B_LFO])) tack_open_page(TACK_PG_ALGO);
        }
        if (pressed & (1u << panel.btn[B_SEQ])) tack_go_home();
    } else if (pg == TACK_PG_FX) {
        if (tack_is_locked() && (k1 || k2 || k3 || k4)) {
            tack_flash("LOCKED");
        } else if (!tack_is_locked() && k1) {
            int32_t r = (int32_t)tack_fx_sel + (k1 > 0 ? 1 : -1);
            if (r < 0) r = 13;
            if (r > 13) r = 0;
            tack_fx_sel = (uint8_t)r;
        }
        if (!tack_is_locked() && k2) {
            int32_t step = k2 > 0 ? 4 : -4;
            switch (tack_fx_sel) {
            case 0:
                tack_fx_dly = (uint8_t)clamp((int32_t)tack_fx_dly + step, 0, 127);
                tack_apply_fx_amounts();
                break;
            case 1:
                song.g[G_DFDBK] = (int16_t)clamp(song.g[G_DFDBK] + step, 0, 120);
                /* FDBK is silent when delay wet is 0 — engage a hearable send. */
                if (song.g[G_DFDBK] > 0 && tack_fx_dly < 24u) {
                    tack_fx_dly = 24;
                    tack_apply_fx_amounts();
                }
                break;
            case 2:
                tack_fx_rev = (uint8_t)clamp((int32_t)tack_fx_rev + step, 0, 127);
                tack_apply_fx_amounts();
                break;
            case 3:
                tack_fx_cho = (uint8_t)clamp((int32_t)tack_fx_cho + step, 0, 127);
                tack_apply_fx_amounts();
                if (tack_shimmer) tack_apply_shimmer();
                break;
            case 4:
                tack_shimmer = (uint8_t)clamp((int32_t)tack_shimmer + (k2 > 0 ? 1 : -1), 0, 3);
                tack_apply_shimmer();
                break;
            case 5: /* FLANGER — voice path IT_FLANGER */
                tack_fx_flange = (uint8_t)clamp((int32_t)tack_fx_flange + step, 0, 127);
                tack_apply_voice_inserts();
                tack_flash("FLANGER");
                break;
            case 6: /* BIT REDUC — voice path IT_CRUSH (!= Drums CRUSH) */
                tack_fx_crush = (uint8_t)clamp((int32_t)tack_fx_crush + step, 0, 127);
                tack_apply_voice_inserts();
                tack_flash("BIT REDUC");
                break;
            case 7: /* CHORUS — voice path IT_CHORUS (beside flanger/bitred) */
                tack_fx_vcho = (uint8_t)clamp((int32_t)tack_fx_vcho + step, 0, 127);
                tack_apply_voice_inserts();
                tack_flash("V CHO");
                break;
            case 8: /* CHAOS — KAOSS combo fold/fuzz+phaser+filter (!= GEN CHAOS dens wobble) */
                tack_fx_chaos = (uint8_t)clamp((int32_t)tack_fx_chaos + step, 0, 127);
                tack_apply_voice_inserts();
                tack_flash("CHAOS");
                break;
            case 9: /* TAPE DLY — short tape delay */
                tack_fx_tape = (uint8_t)clamp((int32_t)tack_fx_tape + step, 0, 127);
                tack_apply_voice_inserts();
                tack_flash("TAPE DLY");
                break;
            case 10:
                tack_drm_dist = (uint8_t)clamp((int32_t)tack_drm_dist + step, 0, 100);
                tack_apply_drum_bus();
                break;
            case 11:
                tack_drm_comp = (uint8_t)clamp((int32_t)tack_drm_comp + step, 0, 100);
                tack_apply_drum_bus();
                break;
            case 12:
                tack_air_freeze ^= 1u;
                tack_flash(tack_air_freeze ? "AIR FREEZE" : "AIR LIVE");
                break;
            default:
                tack_air_morph = (uint8_t)clamp((int32_t)tack_air_morph + (k2 > 0 ? 1 : -1), 0, 3);
                tack_flash("AIR MORPH");
                break;
            }
            /* No generic "FX SET" — specific row flash (or quiet) only. */
        }
        if (!tack_is_locked() && k3 && (uint32_t)(fm1_ms - tack_k3_ms) > 250u) {
            tack_k3_ms = fm1_ms;
            tack_pong ^= 1u;
            tack_apply_pong();
            tack_flash(tack_pong ? "PONG ON" : "PONG OFF");
        }
        if (!tack_is_locked() && k4) {
            /* Named FX scenes Dry/Room/Dub/Shimmer — bar-end while playing; independent of style. */
            uint8_t sc = (uint8_t)((tack_fx_scene + (k4 > 0 ? 1 : 3)) & 3u);
            tack_queue_fxsc(sc);
        }
        if (pressed & (1u << panel.btn[B_SEQ])) tack_go_home();
    
    } else if (pg == TACK_PG_LEARN) {
        /* 5.8 MIDI learn — arm CC, browse targets, save map; continuous CC silent */
        if (!tack_is_locked() && k1) {
            int32_t t = (int32_t)tack_learn_target + (k1 > 0 ? 1 : -1);
            if (t < 0) t = 7; if (t > 7) t = 0;
            tack_learn_target = (uint8_t)t;
        }
        if (!tack_is_locked() && k2 && (uint32_t)(fm1_ms - tack_k2_ms) > 180u) {
            tack_k2_ms = fm1_ms;
            tack_learn_armed = 1u;
            tack_flash("ARM CC");
        }
        if (k3) {
            /* 5.9 horse-proof: clear blocked while PLAY or LOCK */
            if (tack_learn_destructive_ok()) {
                tack_learn_map[tack_learn_target % 8u] = 0xFFu;
                tack_flash("CLR MAP");
            }
        }
        if (!tack_is_locked() && k4 && (uint32_t)(fm1_ms - tack_k2_ms) > 180u) {
            tack_k2_ms = fm1_ms;
            tack_flash("MAP SAVED");
        }
        if (pressed & (1u << panel.btn[B_SEQ])) tack_go_home();

    } else if (pg == TACK_PG_ZEN) {
        /* 6.2 Zen morph — K1 = CALM maps filter+reverb+level; K2/K4 exit */
        if (k1) {
            int32_t v = (int32_t)tack_zen_calm + (k1 > 0 ? 4 : -4);
            if (v < 0) v = 0; if (v > 127) v = 127;
            tack_zen_calm = (uint8_t)v;
            tack_zen_apply();
        }
        if ((k2 || k4) && (uint32_t)(fm1_ms - tack_k2_ms) > 300u) {
            tack_k2_ms = fm1_ms;
            tack_zen_exit();
        }
        if (pressed & (1u << panel.btn[B_SEQ])) tack_go_home();

    } else if (pg == TACK_PG_FEEL) {
        /* 6.2: MUTE/SOLO ignore LOCK (non-destructive). Other rows still LOCK-gated.
         * 6.6: ARP row 9 — dedicated mode toggle/cycle (also physical ARP button). */
        uint8_t feel_ms = (tack_feel_sel == 10u || tack_feel_sel == 11u);
        if (tack_is_locked() && (k1 || k2 || k3 || k4) && !feel_ms) {
            tack_flash("LOCKED");
        }
        if ((!tack_is_locked() || feel_ms) && k1) {
            int32_t r = (int32_t)tack_feel_sel + (k1 > 0 ? 1 : -1);
            if (r < 0) r = 11;
            if (r > 11) r = 0;
            tack_feel_sel = (uint8_t)r;
        }
        /* MUTE/SOLO: fire even while LOCKED (rows 10/11) */
        if ((tack_feel_sel == 10u || tack_feel_sel == 11u) && k2 && (uint32_t)(fm1_ms - tack_k2_ms) > 160u) {
            tack_k2_ms = fm1_ms;
            if (tack_feel_sel == 10u) {
                tack_mute ^= (uint8_t)(1u << (tack_mute_sel & 3u));
                tack_apply_mute_solo();
                tack_flash(tack_mute & (1u << (tack_mute_sel & 3u)) ? "MUTE" : "UNMUTE");
            } else {
                tack_solo ^= (uint8_t)(1u << (tack_mute_sel & 3u));
                tack_apply_mute_solo();
                tack_flash(tack_solo & (1u << (tack_mute_sel & 3u)) ? "SOLO" : "SOLO OFF");
            }
        }
        if (!tack_is_locked() && k2 && (uint32_t)(fm1_ms - tack_k2_ms) > 200u) {
            tack_k2_ms = fm1_ms;
            switch (tack_feel_sel) {
            case 0: break; /* dens via K3 */
            case 1: tack_fill ^= 1u; if (tack_drm_part_own) tack_drm_phrase_fills(); else tack_patterns(); tack_restart(); tack_flash(tack_fill ? "FILL ON" : "FILL OFF"); break;
            case 2: tack_chaos ^= 1u; tack_flash(tack_chaos ? "CHAOS ON" : "CHAOS OFF"); break;
            case 3: tack_roll_take(); break;
            case 4: tack_do_undo(); break;
            case 5: tack_auto_set(tack_auto ^ 1u); break;
            case 6: { /* CLK INT/USB/TRS — Logic / MIDI clock sync */
                uint8_t clk = (uint8_t)((song.g[G_CLOCK] + 1) % 3);
                song.g[G_CLOCK] = (int16_t)clk;
                tack_flash(clk == 0 ? "CLK INT" : clk == 1 ? "CLK USB" : "CLK TRS");
                break;
            }
            case 7: /* LVL — cycle lane then adjust via K3 */
                tack_mute_sel = (uint8_t)((tack_mute_sel + 1u) & 3u);
                tack_flash2("LVL ", TACK_LANE[tack_mute_sel]);
                break;
            case 8: /* PAN — cycle lane */
                tack_mute_sel = (uint8_t)((tack_mute_sel + 1u) & 3u);
                tack_flash2("PAN ", TACK_LANE[tack_mute_sel]);
                break;
            case 9: /* ARP toggle for focused lane */
                tack_arp_toggle();
                break;
            case 10: case 11: break; /* MUTE/SOLO handled above */
            default: break;
            }
        }
        /* ARP: K3 cycles mode UP/DN/... ; MUTE/SOLO: K3 cycles lane */
        if (tack_feel_sel == 9u && k3 && !tack_is_locked()) {
            tack_arp_cycle_mode(k3);
        }
        if ((tack_feel_sel == 10u || tack_feel_sel == 11u) && k3) {
            tack_mute_sel = (uint8_t)((tack_mute_sel + (k3 > 0 ? 1 : 3)) & 3u);
            tack_flash2(tack_feel_sel == 10u ? "MUTE " : "SOLO ", TACK_LANE[tack_mute_sel]);
        }
        if (!tack_is_locked() && (k3 || (tack_feel_sel == 0 && k2) || ((tack_feel_sel == 7 || tack_feel_sel == 8) && k2 && (uint32_t)(fm1_ms - tack_k2_ms) <= 200u))) {
            int32_t step = (k3 ? k3 : k2) > 0 ? 4 : -4;
            if (tack_feel_sel == 0) {
                int16_t d = (int16_t)tack_dens + (int16_t)step;
                int dir = step > 0 ? 1 : -1;
                if (d < 0) d = 0;
                if (d > 100) d = 100;
                if ((uint8_t)d != tack_dens) {
                    tack_dens = (uint8_t)d;
                    if (tack_drm_part_own) tack_drm_density_nudge(dir);
                    else { tack_patterns(); tack_flash("DENS"); }
                }
            } else if (tack_feel_sel == 7 && k3) {
                tack_set_lane_level(tack_mute_sel, (int16_t)step);
                {
                    char vb[8];
                    fmt_int(vb, tack_level[tack_mute_sel & 3u]);
                    tack_flash2("LVL ", vb);
                }
            } else if (tack_feel_sel == 8 && k3) {
                tack_set_lane_pan(tack_mute_sel, (int16_t)step);
                {
                    char vb[8];
                    int8_t pv = tack_lane_pan[tack_mute_sel & 3u];
                    if (pv < 0) { vb[0] = '-'; fmt_int(vb + 1, (int16_t)(-pv)); }
                    else fmt_int(vb, (int16_t)pv);
                    tack_flash2("PAN ", vb);
                }
            }
        }
        if (pressed & (1u << panel.btn[B_SEQ])) {
            if (!tack_is_locked() && tack_feel_sel == 3) tack_roll_take();
            else if (!tack_is_locked() && tack_feel_sel == 4) tack_do_undo();
            else tack_go_home();
        }
        /* 6.19: on ROLL row, K4 fires GEN (≠ chord ROLL; ≠ MORE·K4 remake) */
        if (!tack_is_locked() && tack_feel_sel == 3u && k4
            && (uint32_t)(fm1_ms - tack_k4_ms) > 280u) {
            tack_k4_ms = fm1_ms;
            tack_drm_gen();
        }
        if (!tack_is_locked() && (pressed & (1u << panel.btn[B_REC]))) {
            /* Feel: HOLD (HOME REC is chord recorder). */
            tack_hold ^= 1u;
            tack_hold_bar = tack_seq_bar();
            tack_restart();
            tack_flash(tack_hold ? "HOLD ON" : "HOLD OFF");
        } else if (tack_is_locked() && (pressed & (1u << panel.btn[B_REC]))) {
            tack_flash("LOCKED");
        }
        /* Ambient fill still thickens drums when toggled from Feel */
        if (tack_style_idx == TACK_STYLE_AMBIENT) {
            tack_mute &= (uint8_t)~(1u << 3);
            if (tack_fill) { trk[3].p[P_LEVEL] = 88; tack_level[3] = 88; }
            else { trk[3].p[P_LEVEL] = 40; tack_level[3] = 40; }
        }

    } else if (pg == TACK_PG_LOOP) {
        /* 6.0 LOOP — Rec / Loop / Quantize / Rhythm. APPLY = K4. All post-OK opt-in. */
        if (tack_is_locked() && (k1 || k2 || k3 || k4)) {
            tack_flash("LOCKED");
        }
        if (!tack_is_locked() && k1) {
            int32_t r = (int32_t)tack_loop_sel + (k1 > 0 ? 1 : -1);
            if (r < 0) r = 4;
            if (r > 4) r = 0;
            tack_loop_sel = (uint8_t)r;
        }
        if (!tack_is_locked() && k2 && (uint32_t)(fm1_ms - tack_k2_ms) > 180u) {
            tack_k2_ms = fm1_ms;
            switch (tack_loop_sel) {
            case 0: /* REC — arm / stop→HELD (never auto-LOOP) */
                tack_crec_tap();
                break;
            case 1: /* LOOP — opt-in phrase loop */
                tack_crec_loop_tap();
                break;
            case 2: /* QUANTIZE — opt-in after REC OK */
                if (tack_crec_mode == 1u) { tack_flash("STOP REC"); break; }
                tack_crec_qnt ^= 1u;
                if (tack_crec_qnt && tack_crec_len) {
                    if (!tack_crec_steps_ready) tack_crec_ms_to_steps();
                    tack_crec_requantize();
                }
                tack_flash(tack_crec_qnt ? "QNT ON" : "QNT OFF");
                break;
            case 3: /* RHYTHM — opt-in template on held phrase */
                if (tack_crec_mode == 1u) { tack_flash("STOP REC"); break; }
                tack_crec_rhy = (uint8_t)((tack_crec_rhy + 1u) % 7u);
                if (tack_crec_len) {
                    if (!tack_crec_steps_ready) tack_crec_ms_to_steps();
                    tack_crec_apply_rhythm();
                }
                tack_flash2("RHY ", TACK_RHY_NAME[tack_crec_rhy]);
                break;
            case 4: /* EXPORT — arm snapshot for Mac outbox JSON */
                tack_export_phrase_arm();
                break;
            default: break;
            }
        }
        if (!tack_is_locked() && k3) {
            if (tack_loop_sel == 3) {
                /* Browse rhythm — apply only when user turns the dial (opt-in). */
                int32_t r = (int32_t)tack_crec_rhy + (k3 > 0 ? 1 : -1);
                if (r < 0) r = 6;
                if (r > 6) r = 0;
                if (tack_crec_mode == 1u) { tack_flash("STOP REC"); }
                else {
                    tack_crec_rhy = (uint8_t)r;
                    if (tack_crec_len) {
                        if (!tack_crec_steps_ready) tack_crec_ms_to_steps();
                        tack_crec_apply_rhythm();
                    }
                    tack_flash2("RHY ", TACK_RHY_NAME[tack_crec_rhy]);
                }
            } else if (tack_loop_sel == 0 && tack_crec_len > 0u) {
                /* REC/HELD row: nudge phrase playhead (preview index) */
                if (k3 > 0)
                    tack_crec_pos = (uint8_t)((tack_crec_pos + 1u) % tack_crec_len);
                else
                    tack_crec_pos = (uint8_t)((tack_crec_pos + tack_crec_len - 1u) % tack_crec_len);
                tack_flash("POS");
            } else if (tack_loop_sel == 1 && tack_crec_len > 0u) {
                /* LOOP row: nudge playhead */
                if (k3 > 0)
                    tack_crec_pos = (uint8_t)((tack_crec_pos + 1u) % tack_crec_len);
                else
                    tack_crec_pos = (uint8_t)((tack_crec_pos + tack_crec_len - 1u) % tack_crec_len);
                tack_flash("POS");
            } else if (tack_loop_sel == 2) {
                /* QUANTIZE row: K3 = opt-in mode/style browse + rebind */
                int32_t st = (int32_t)tack_style_idx + (k3 > 0 ? 1 : -1);
                if (st < 0) st = (int32_t)TACK_STYLE_COUNT - 1;
                if (st >= (int32_t)TACK_STYLE_COUNT) st = 0;
                tack_pick_style((uint8_t)st);
                tack_style_rebind();
                tack_flash("MODE");
            }
        }
        if (!tack_is_locked() && k4 && (uint32_t)(fm1_ms - tack_k4_ms) > 250u) {
            tack_k4_ms = fm1_ms;
            /* K4 = APPLY held phrase → seq (opt-in). Long path: USER load if empty phrase. */
            if (tack_crec_len && tack_crec_mode != 1u) {
                tack_crec_apply_to_seq();
            } else if (TACK_USER_PHRASE_COUNT) {
                uint8_t n = (uint8_t)((tack_user_phrase_sel + 1u) % TACK_USER_PHRASE_COUNT);
                tack_user_phrase_load(n);
            } else {
                tack_flash("NO PHRASE");
            }
        }
        /* Physical REC on LOOP: short = cycle, long = clear */
        if (pressed & (1u << panel.btn[B_REC])) {
            tack_crec_down_ms = fm1_ms ? fm1_ms : 1u;
            tack_crec_long_done = 0;
        }
        if (tack_crec_down_ms && !tack_crec_long_done
            && (fm1_in.buttons & (1u << panel.btn[B_REC]))
            && (uint32_t)(fm1_ms - tack_crec_down_ms) >= 600u) {
            tack_crec_long_done = 1;
            if (tack_is_locked()) tack_flash("LOCKED");
            else tack_crec_clear();
        }
        if (released & (1u << panel.btn[B_REC])) {
            if (!tack_crec_long_done) {
                if (tack_is_locked()) tack_flash("LOCKED");
                else tack_crec_tap();
            }
            tack_crec_down_ms = 0;
        }
        if (pressed & (1u << panel.btn[B_SEQ])) tack_go_home();
        /* SAVE while PLAY+crec PLAY still live-erase (same as Home) */
        if (!tack_is_locked() && (pressed & (1u << panel.btn[B_SAVE]))
            && tack_crec_mode != 1u && tack_crec_len && song.playing) {
            tack_crec_live_erase();
        }
    } else if (pg == TACK_PG_GEN) {
        /* Generative — live Auto / Chaos / dens / morph / freeze / variation.
         * Chaos defaults OFF. Sparse: nothing random unless Chaos is on. */
        if (tack_is_locked() && (k1 || k2 || k3 || k4)) {
            tack_flash("LOCKED");
        }
        if (!tack_is_locked() && k1) {
            int32_t r = (int32_t)tack_gen_sel + (k1 > 0 ? 1 : -1);
            if (r < 0) r = 6;
            if (r > 6) r = 0;
            tack_gen_sel = (uint8_t)r;
        }
        if (!tack_is_locked() && k2 && (uint32_t)(fm1_ms - tack_k2_ms) > 180u) {
            tack_k2_ms = fm1_ms;
            switch (tack_gen_sel) {
            case 0: tack_auto_set(tack_auto ^ 1u); break;
            case 1:
                tack_chaos ^= 1u;
                tack_flash(tack_chaos ? "CHAOS ON" : "CHAOS OFF");
                break;
            case 2: break; /* dens via K3 */
            case 3:
                tack_drm_spare ^= 1u;
                tack_patterns();
                tack_flash(tack_drm_spare ? "SPARE ON" : "SPARE OFF");
                break;
            case 4: break; /* morph via K3 */
            case 5:
                tack_air_freeze ^= 1u;
                tack_flash(tack_air_freeze ? "AIR FREEZE" : "AIR LIVE");
                break;
            case 6: break; /* variation via K3 */
            default: break;
            }
        }
        if (!tack_is_locked() && k3) {
            if (tack_gen_sel == 2) {
                int16_t d = (int16_t)tack_dens + (k3 > 0 ? 4 : -4);
                int dir = k3 > 0 ? 1 : -1;
                if (d < 0) d = 0;
                if (d > 100) d = 100;
                if ((uint8_t)d != tack_dens) {
                    tack_dens = (uint8_t)d;
                    if (tack_drm_part_own) tack_drm_density_nudge(dir);
                    else { tack_patterns(); tack_flash("DENS"); }
                }
            } else if (tack_gen_sel == 4) {
                tack_air_morph = (uint8_t)clamp((int32_t)tack_air_morph + (k3 > 0 ? 1 : -1), 0, 3);
                tack_flash("AIR MORPH");
            } else if (tack_gen_sel == 6) {
                /* Variation: 0=off 1/2/4/8 bars — every-bar phrase breathe amount. */
                static const uint8_t rates[5] = {0u, 1u, 2u, 4u, 8u};
                uint8_t i, cur = 0;
                for (i = 0; i < 5u; i++) if (rates[i] == tack_move_rate) { cur = i; break; }
                if (k3 > 0) cur = (uint8_t)((cur + 1u) % 5u);
                else cur = (uint8_t)((cur + 4u) % 5u);
                tack_move_rate = rates[cur];
                tack_move_phase = 0;
                if (!tack_move_rate) tack_move_variant = 0;
                tack_flash(tack_move_rate ? "VARIATION" : "VAR OFF");
            }
        }
        if (!tack_is_locked() && k4 && (uint32_t)(fm1_ms - tack_k4_ms) > 250u) {
            tack_k4_ms = fm1_ms;
            /* 5.9: GEN K4 → Zen calm (Chaos forced off). LAND via Auto row. */
            tack_zen_enter();
        }
        if (pressed & (1u << panel.btn[B_SEQ])) tack_go_home();
    } else if (pg == TACK_PG_DRUMS) {
        /* 6.9 — PART circle grid (lanes × steps).
         * K1 move step cursor · K2 hit/erase (+audition) · K3 lane row · K4 remake.
         * Independent ifs — K4 never starves. GLO nav only (never remake). */
        if (tack_is_locked() && (k1 || k2 || k3 || k4)) {
            tack_flash("LOCKED");
        } else if (!tack_is_locked() && tack_drm_depth == 0u) {
            /* ---- 6.19 PART: K1 step · K2 hit (MORE·K2 LOCK) · K3 lane · K4 GEN ----
             * ARP-again = REMIX. dens nudge = MORE·K1 (owned PART). GLO nav only. */
            uint8_t bs = tack_bar_steps();
            uint8_t more = (uint8_t)((fm1_in.buttons & (1u << panel.btn[B_SAVE])) ? 1u : 0u);
            if (tack_drm_step >= bs) tack_drm_step = 0;
            if (k1) {
                if (more) {
                    /* MORE·K1 = DENSITY thin/thicken unlocked PART hits */
                    int16_t d = (int16_t)tack_dens + (k1 > 0 ? 4 : -4);
                    if (d < 0) d = 0;
                    if (d > 100) d = 100;
                    if ((uint8_t)d != tack_dens) {
                        int dir = k1 > 0 ? 1 : -1;
                        tack_dens = (uint8_t)d;
                        tack_drm_density_nudge(dir);
                    }
                } else {
                    int32_t r = (int32_t)tack_drm_step + (k1 > 0 ? 1 : -1);
                    if (r < 0) r = (int32_t)bs - 1;
                    if (r >= (int32_t)bs) r = 0;
                    tack_drm_step = (uint8_t)r;
                    {
                        char msg[8];
                        msg[0] = 'S';
                        if (tack_drm_step + 1u >= 10u) {
                            msg[1] = (char)('0' + (tack_drm_step + 1u) / 10u);
                            msg[2] = (char)('0' + (tack_drm_step + 1u) % 10u);
                            msg[3] = 0;
                        } else {
                            msg[1] = (char)('0' + tack_drm_step + 1u);
                            msg[2] = 0;
                        }
                        tack_flash(msg);
                    }
                }
            }
            if (k3) {
                int32_t r = (int32_t)tack_drm_lane + (k3 > 0 ? 1 : -1);
                if (r < 0) r = 7;
                if (r > 7) r = 0;
                tack_drm_lane = (uint8_t)r;
                if (tack_drm_lane_locked(tack_drm_lane))
                    tack_flash2("L ", tack_drm_lane_name(tack_drm_lane));
                else
                    tack_flash(tack_drm_lane_name(tack_drm_lane));
            }
            if (k2 && (uint32_t)(fm1_ms - tack_k2_ms) > 100u) {
                tack_k2_ms = fm1_ms;
                if (more) {
                    tack_drm_lock_toggle();
                } else {
                    uint8_t on;
                    tack_drm_toggle_step(tack_drm_lane, tack_drm_step);
                    on = tack_drm_step_on(tack_drm_lane, tack_drm_step);
                    if (on) tack_drm_audition(tack_drm_lane);
                    tack_flash(on ? "HIT ON" : "HIT OFF");
                }
            }
            if (k4 && (uint32_t)(fm1_ms - tack_k4_ms) > 280u) {
                tack_k4_ms = fm1_ms;
                if (more)
                    tack_drm_remake(); /* MORE·K4 power: style-mask reseed */
                else
                    tack_drm_gen();
            }
        } else if (!tack_is_locked() && tack_drm_depth == 1u) {

            /* ---- FEEL: kit / dens / hats / spare / kick / fill / ratchet / crush / comp ---- */
            if (k1) {
                int32_t r = (int32_t)tack_drm_sel + (k1 > 0 ? 1 : -1);
                if (r < 0) r = 9;
                if (r > 9) r = 0;
                tack_drm_sel = (uint8_t)r;
            }
            if (k2 && (uint32_t)(fm1_ms - tack_k2_ms) > 180u) {
                tack_k2_ms = fm1_ms;
                switch (tack_drm_sel) {
                case 0: {
                    uint8_t cur = 0, i;
                    for (i = 0; i < 4u; i++)
                        if (trk[3].preset == TACK_KIT_PRESET[i]) { cur = i; break; }
                    cur = (uint8_t)((cur + 1u) & 3u);
                    tack_queue_kit(cur);
                    break;
                }
                case 2:
                    tack_drm_hats = (uint8_t)((tack_drm_hats + 1u) % 3u);
                    tack_patterns();
                    tack_flash(tack_drm_hats == 0u ? "HATS QUIET" : tack_drm_hats == 1u ? "HATS NORM" : "HATS MORE");
                    break;
                case 3:
                    tack_drm_spare ^= 1u;
                    tack_patterns();
                    tack_flash(tack_drm_spare ? "SPARE ON" : "SPARE OFF");
                    break;
                case 4:
                    tack_kick_lock ^= 1u;
                    tack_patterns();
                    tack_flash(tack_kick_lock ? "KICK LOCK" : "KICK FREE");
                    break;
                case 5:
                    tack_fill ^= 1u;
                    if (tack_drm_part_own) tack_drm_phrase_fills();
                    else tack_patterns();
                    tack_restart();
                    tack_flash(tack_fill ? "FILL ON" : "FILL OFF");
                    break;
                case 6:
                    tack_drm_ratchet = (uint8_t)((tack_drm_ratchet + 1u) & 3u);
                    tack_patterns();
                    if (!tack_drm_ratchet) tack_flash("RATCH OFF");
                    else if (tack_drm_ratchet == 1u) tack_flash("RATCH x2");
                    else if (tack_drm_ratchet == 2u) tack_flash("RATCH x3");
                    else tack_flash("RATCH x4");
                    break;
                case 7:
                    if (tack_drm_dist < 20u) tack_drm_dist = 36u;
                    else if (tack_drm_dist < 50u) tack_drm_dist = 68u;
                    else if (tack_drm_dist < 80u) tack_drm_dist = 92u;
                    else tack_drm_dist = 12u;
                    tack_apply_drum_bus();
                    tack_flash("CRUSH");
                    break;
                case 8:
                    if (tack_drm_comp < 24u) tack_drm_comp = 40u;
                    else if (tack_drm_comp < 50u) tack_drm_comp = 68u;
                    else if (tack_drm_comp < 80u) tack_drm_comp = 92u;
                    else tack_drm_comp = 16u;
                    tack_apply_drum_bus();
                    tack_flash("COMP");
                    break;
                case 9:
                    /* 6.19 GEN — style-aware PART; ≠ MORE·K4 REMAKE */
                    tack_drm_gen();
                    break;
                default: break;
                }
            }
            if (k3 || (tack_drm_sel == 1 && k2) || ((tack_drm_sel == 7 || tack_drm_sel == 8) && k3)) {
                int32_t step = (k3 ? k3 : k2) > 0 ? 4 : -4;
                if (tack_drm_sel == 7) {
                    tack_drm_dist = (uint8_t)clamp((int32_t)tack_drm_dist + step, 0, 100);
                    tack_apply_drum_bus();
                    tack_flash("CRUSH");
                } else if (tack_drm_sel == 8) {
                    tack_drm_comp = (uint8_t)clamp((int32_t)tack_drm_comp + step, 0, 100);
                    tack_apply_drum_bus();
                    tack_flash("COMP");
                } else if (tack_drm_sel == 1 || k3) {
                    int16_t d = (int16_t)tack_dens + (int16_t)step;
                    int dir = step > 0 ? 1 : -1;
                    if (d < 0) d = 0;
                    if (d > 100) d = 100;
                    if ((uint8_t)d != tack_dens) {
                        tack_dens = (uint8_t)d;
                        if (tack_drm_part_own) tack_drm_density_nudge(dir);
                        else { tack_patterns(); tack_flash("DENS"); }
                    }
                }
            }
            /* K4 on FEEL → back to PART (never remake from FEEL K4) */
            if (k4 && (uint32_t)(fm1_ms - tack_k4_ms) > 280u) {
                tack_k4_ms = fm1_ms;
                tack_drm_depth = 0u;
                tack_flash("PART");
            }
        }

        /* 6.10: no per-frame ambient level slap on GLO/drums page (was mutating drums on entry). */
        if (pressed & (1u << panel.btn[B_SEQ])) tack_go_home();


    }


    /* 6.10: short ARP → open Drums PART. Already on PART → remake. Home long-ARP = BRIDGE.
     * Off-home long-ARP = mute. Arpeggiator lives on FEEL (ENV) ARP row. */
    if ((released & (1u << panel.btn[B_ARP])) && tack_k3_down_ms) {
        if (!tack_k3_long_done) {
            if (tack_is_locked())
                tack_flash("LOCKED");
            else
                tack_open_drums_part();
        }
        tack_k3_down_ms = 0;
    }
    /* Non-home: ARP press arms short/long so release opens PART too. */
    if (pressed & (1u << panel.btn[B_ARP])) {
        if (tack_page != TACK_PG_HOME) {
            tack_k3_down_ms = fm1_ms ? fm1_ms : 1u;
            tack_k3_long_done = 0;
        }
    }
    /* Non-home long ARP ≥450ms → mute (secondary; FEEL still primary mute). */
    if (tack_page != TACK_PG_HOME && tack_k3_down_ms && !tack_k3_long_done
        && (fm1_in.buttons & (1u << panel.btn[B_ARP]))
        && (uint32_t)(fm1_ms - tack_k3_down_ms) >= 450u) {
        tack_k3_long_done = 1;
        tack_mute ^= (uint8_t)(1u << tack_mute_sel);
        tack_flash(tack_mute & (1u << tack_mute_sel) ? "MUTE" : "UNMUTE");
        tack_apply_mute_solo();
    }
    /* PLAY/STOP: tap toggles; hold ≥600ms = Panic (stop + clear stuck notes). */
    if (pressed & (1u << panel.btn[B_PLAY])) {
        tack_play_down_ms = fm1_ms ? fm1_ms : 1u;
        tack_play_long_done = 0;
    }
    if (tack_play_down_ms && !tack_play_long_done
        && (fm1_in.buttons & (1u << panel.btn[B_PLAY]))
        && (uint32_t)(fm1_ms - tack_play_down_ms) >= 600u) {
        tack_play_long_done = 1;
        tack_panic();
    }
    if ((released & (1u << panel.btn[B_PLAY])) && tack_play_down_ms) {
        if (!tack_play_long_done) {
            if (song.playing) { seq_stop(); tack_flash("STOP"); }
            else {
                if (tack_auto) tack_auto_begin_cycle();
                seq_start();
                tack_flash(tack_auto ? (tack_user_chose ? "AUTO PLAY" : "AUTO HOME") : "PLAY");
            }
        }
        tack_play_down_ms = 0;
    }

    /* Tempo-grid clock: chord moves on 16th grid (not only bar boundary). */
    {
        uint8_t bar_now = tack_seq_bar();
        uint8_t step16 = tack_seq_step16();
        uint8_t abs_step = (uint8_t)(trk[0].seq_idx % tack_loop_steps()); /* 4 bars × meter */

        /* Opt-in LOOP: free-time phrase playback (ms clock), not seq-step locked. */
        if (tack_crec_mode == 2u && tack_crec_len > 0u && !tack_hold) {
            uint32_t span = tack_crec_span_ms ? tack_crec_span_ms : (tack_crec_ms[tack_crec_len - 1u] + 400u);
            uint32_t elapsed, i, best;
            if (span < 1u) span = 1u;
            if (!song.playing) seq_start();
            elapsed = (fm1_ms >= tack_crec_loop_t0) ? (fm1_ms - tack_crec_loop_t0) : 0u;
            elapsed = elapsed % span;
            best = 0u;
            for (i = 0u; i < tack_crec_len; i++) {
                if (tack_crec_ms[i] <= elapsed) best = i;
            }
            if ((uint8_t)best != tack_crec_last_fire) {
                tack_state.chords[bar_now] = tack_crec[best];
                tack_bar_apply_literal(bar_now, (uint8_t)best);
                tack_crec_pos = (uint8_t)best;
                tack_crec_last_fire = (uint8_t)best;
                tack_pending = 1;
            }
        }

        /* Auto: apply pending move when playhead reaches living grid target. */
        if (tack_chord_grid_armed && !tack_hold && !tack_crec_mode
            && song.playing && step16 == tack_chord_grid
            && abs_step != tack_auto_last_step) {
            tack_auto_last_step = abs_step;
            tack_chord_grid_armed = 0;
            tack_auto_steer(bar_now);
            /* Arm next landing on a different grid slot for variation. */
            tack_chord_grid = tack_auto_next_grid();
            tack_chord_grid_armed = tack_auto ? 1u : 0u;
        }

        if (bar_now != tack_last_bar) {
            /* Bar-end / cycle-quantize: Style / Sounds / kit / FX scene. */
            tack_flush_bar_queues();
            /* Arm Auto grid at bar wrap if idle (first landing this bar). */
            if (tack_auto && !tack_hold && !tack_crec_mode && !tack_chord_grid_armed) {
                tack_chord_grid = tack_auto_next_grid();
                tack_chord_grid_armed = 1u;
            }
            /* Legacy: if Auto somehow unarmed mid-bar, still nudge once per bar. */
            if (tack_auto && !tack_hold && !tack_crec_mode && !tack_chord_grid_armed)
                tack_auto_steer(bar_now);
            if (bar_now == 0u) {
                tack_loop++;
                tack_pending = 1;
                tack_chaos_tick();
                tack_air_morph_tick();
                /* A/B arrange: each finished 4-chord loop → next section column. */
                if (tack_arr_on && song.playing && !tack_hold)
                    tack_arr_advance();
            }
            if (tack_move_rate) {
                tack_move_phase++;
                if ((tack_move_phase % tack_move_rate) == 0u) {
                    tack_move_variant ^= 1u;
                    tack_pending = 1;
                }
            }
            tack_last_bar = bar_now;
        }
        /* Flush pending patterns on bar OR mid-bar grid chord move (Auto/PLAY). */
        if (tack_pending) {
            tack_patterns();
            tack_pending = 0;
        }
    }
    tack_midi_loop_tick();

}

void tack_app_draw(void)
{
    char a[36], b[24], barLabel[4], bpm[8];
    TackChord moves[TACK_MOVES];
    uint32_t i, y, bar;
    uint8_t pg = tack_page;
    track_t *tf = &trk[tack_mute_sel & 3u];
    uint8_t action_live;
    uint8_t need_full;

    if (!tack_ready) tack_app_init();

    /* 6.16/6.17: boot OLED — big TACK, then Home. One black fill; skip on input.
     * Timeout path clears tack_boot_until = 0 (belt with input expiry). */
    if (tack_boot_until) {
        if ((int32_t)(tack_boot_until - fm1_ms) > 0) {
            const char *L = "TACK";
            int32_t gap = 10;
            int32_t lw[4], total = 0, x0, li;
            if (!tack_boot_painted) {
                lcd_fill(0, 0, 240, 240, T_BG);
                tack_boot_painted = 1;
            }
            for (li = 0; li < 4; li++) {
                char ch[2] = { L[li], 0 };
                lw[li] = text_w(&AF_L, ch);
                total += lw[li];
            }
            total += gap * 3;
            x0 = (240 - total) / 2;
            if (x0 < 0) x0 = 0;
            for (li = 0; li < 4; li++) {
                char ch[2] = { L[li], 0 };
                draw_text_line((uint32_t)x0, 96u, (uint32_t)lw[li] + 4u, &AF_L, ch, T_THEME, T_BG, 0);
                x0 += lw[li] + gap;
            }
            draw_text_line(0, 140, 240, &AF_S, "HARMONY PILOT", T_DIM, T_BG, 1);
            draw_text_line(0, 158, 240, &AF_S, "6.28-rc5", T_MID, T_BG, 1);
            return;
        }
        tack_boot_until = 0;
        tack_drawn = 0; /* leave splash → Home full-draws once */
    }
    bar = tack_seq_bar();
    fmt_int(bpm, song.g[G_BPM]);
    action_live = (tack_action[0] && (int32_t)(tack_action_until - fm1_ms) > 0) ? 1u : 0u;

    /* Stable redraw: page/content sig ignores playhead bar on edit pages, and
     * action toast expiry alone never forces a full-screen clear (less blink). */
    {
        static uint8_t last_pg = 0xFFu;
        static uint32_t live_sig;
        uint32_t sig = ((uint32_t)tack_sel << 3) | ((uint32_t)song.playing << 6) |
                       ((uint32_t)tack_mute << 7) | ((uint32_t)tack_mute_sel << 11) |
                       ((uint32_t)tack_mode_idx << 13) | ((uint32_t)tack_move_rate << 18) |
                       ((uint32_t)tack_move_variant << 22) | ((uint32_t)tack_auto << 23) |
                       ((uint32_t)tack_hold << 24) | ((uint32_t)tack_hold_bar << 25) |
                       ((uint32_t)tack_style_idx << 27) | ((uint32_t)tack_fill << 31) |
                       ((uint32_t)(uint8_t)(tack_octave + 3) << 20);
        /* Home only: bar drives chord highlight — edit pages ignore playhead. */
        if (pg == TACK_PG_HOME) sig ^= (uint32_t)bar ^ ((uint32_t)tack_pick_slot << 2);
        sig = sig * 33u + tack_action_serial; /* toast text change = user action */
        sig = sig * 33u + (uint32_t)pg * 17u + tack_snd_bank * 3u + tack_fx_sel
            + tack_feel_sel * 5u + tack_gen_sel * 6u + tack_chaos * 7u + tack_pong * 11u + tack_shimmer * 13u + tack_air_freeze * 17u + tack_air_morph * 19u
            + (uint32_t)tack_move_rate * 151u + (uint32_t)tack_drm_spare * 157u
            + (uint32_t)song.g[G_BPM] + (uint32_t)tack_dens * 19u
            + (uint32_t)tf->eng_req + (uint32_t)tf->preset * 23u
            + (uint32_t)tack_style_cursor * 29u
            + (uint32_t)tack_drm_dist * 31u + (uint32_t)tack_drm_comp * 37u
            + (uint32_t)tack_fx_dly + (uint32_t)tack_fx_rev * 41u + (uint32_t)tack_fx_cho * 43u
            + (uint32_t)tack_amb_tex * 47u + (uint32_t)tack_snd_cursor * 53u + (uint32_t)tack_snd_depth * 59u + (uint32_t)tack_snd_pick * 61u + (uint32_t)tack_trk_lib_pick[0] * 73u + (uint32_t)tack_trk_lib_pick[1] * 79u + (uint32_t)tack_trk_lib_pick[2] * 83u + (uint32_t)tack_trk_lib_pick[3] * 89u + (uint32_t)tack_fx_flange * 67u + (uint32_t)tack_fx_crush * 71u + (uint32_t)tack_fx_chaos * 97u
            + (uint32_t)tack_drm_sel * 59u + (uint32_t)tack_drm_hats * 61u
            + (uint32_t)tack_drm_spare * 67u + (uint32_t)tack_kick_lock * 71u
            + (uint32_t)tack_beat_kit * 73u
            + (uint32_t)tack_drm_depth * 163u + (uint32_t)tack_drm_lane * 167u + (uint32_t)tack_rand_drm * 173u + (uint32_t)tack_drm_lock * 181u
            + (uint32_t)tack_drm_step * 171u
            + ((pg == TACK_PG_DRUMS && !tack_drm_depth && song.playing)
                   ? (uint32_t)tack_seq_step16() * 191u : 0u)
            + (uint32_t)trk[tack_mute_sel & 3u].p[P_AMODE] * 173u
            + (uint32_t)trk[3].step[tack_drm_step % 16u].hit * 179u
            + (uint32_t)tack_crec_mode * 59u + (uint32_t)tack_crec_len * 61u
            + (uint32_t)tack_crec_qnt * 67u + (uint32_t)tack_loop_sel * 101u
            + (uint32_t)tack_crec_rhy * 103u
            + (uint32_t)tack_ab_live * 79u + (uint32_t)tack_arr_pos * 83u
            + (uint32_t)tack_arr_len * 89u + (uint32_t)tack_arr_on * 97u
            + (uint32_t)tack_arr_edit * 101u + (uint32_t)tack_arr_sel * 103u
            + (uint32_t)tack_midi_master * 107u + (uint32_t)tack_midi_expr * 109u
            + (uint32_t)tack_set_sel * 113u + (uint32_t)tack_set_live * 127u
            + (uint32_t)tack_q_perf * 131u + (uint32_t)tack_show_lock * 137u
            + (uint32_t)tack_fx_scene * 139u + (uint32_t)tack_is_locked() * 149u
            + (uint32_t)tack_set_browse * 151u + (uint32_t)(tack_home_show_more() ? 1u : 0u) * 157u
            + (uint32_t)(tack_more_until & 0xFFu);
        for (uint32_t si = 0; si < 4u; si++)
            sig = sig * 33u + (uint32_t)tack_state.chords[si].root * 17u + tack_state.chords[si].quality;
        for (uint32_t ai = 0; ai < TACK_ARR_MAX; ai++)
            sig = sig * 33u + (uint32_t)tack_arr[ai];
        for (uint32_t pi = 0; pi < TACK_PERF_N; pi++)
            sig = sig * 33u + (uint32_t)tack_perf[pi].used + (uint32_t)tack_perf[pi].style * 3u;
        for (uint32_t pi = 0; pi < 2u; pi++)
            for (uint32_t ci = 0; ci < 4u; ci++)
                sig = sig * 33u + (uint32_t)tack_phrase[pi][ci].root * 13u + tack_phrase[pi][ci].quality;
        {
            uint32_t live = ((uint32_t)action_live << 1) | (uint32_t)bar;
            if (tack_drawn && sig == tack_sig && live == live_sig) return;
            /* 5.7/6.12 kill blink: full framebuffer wipe ONLY on first draw / page change.
             * Content picks (lib/preset/PART cursor/K1–K4/GLO depth/ARP) soft-redraw — never tack_drawn=0. */
            need_full = (!tack_drawn || pg != last_pg) ? 1u : 0u;
            tack_sig = sig;
            live_sig = live;
            last_pg = pg;
            tack_drawn = 1;
        }
    }

    /* Full clear ONLY on page change / init — never on content sig (kill blink).
     * Soft = HEADER ONLY. Body overdraws; Home bottom uses dirty-region wipe. */
    if (need_full)
        lcd_fill(0, 0, 240, 240, T_BG);
    else
        lcd_fill(0, 0, 240, 22, T_BG); /* header only — accents stay */

    /* ---- shared header ---- */
    draw_text_line(10, 6, 100, &AF_M, TACK_PG_TITLE[pg], tack_page_col(pg), T_BG, 0);
    {
        char st[6]; uint8_t sn = 0;
        if (tack_fill) st[sn++] = 'F';
        if (tack_auto) st[sn++] = 'A';
        if (tack_hold) st[sn++] = 'H';
        if (tack_chaos) st[sn++] = 'C';
        if (tack_air_freeze) st[sn++] = 'Z'; /* freeze air — distinct from Fill */
        if (tack_pong) st[sn++] = 'P';
        if (tack_is_locked()) st[sn++] = 'L';
        st[sn] = 0;
        if (sn) draw_text_line(108, 8, 28, &AF_S, st, T_ACCENT, T_BG, 0);
    }
    if (action_live)
        draw_text_line(138, 8, 60, &AF_S, tack_action, T_THEME, T_BG, 2);
    else {
        str_cpy(a, bpm, sizeof a);
        tack_str_cat(a, " BPM", sizeof a);
        draw_text_line(138, 8, 60, &AF_S, a, T_MID, T_BG, 2);
    }
    barLabel[0] = 'B'; barLabel[1] = (char)('1' + bar); barLabel[2] = 0;
    /* PLAY lit in accent; STOP stays quiet — one hot color, no flicker. */
    draw_text_line(200, 8, 32, &AF_S, song.playing ? "PLAY" : "STOP",
                   song.playing ? T_ACCENT : T_DIM, T_BG, 2);
    cv_rect(8, 20, 224, 1, T_TINT); /* calm header rule */

    /* Always fall through and repaint page chrome (HOME pick rails + STYLE/SOUNDS
     * T_SEL/T_ACCENT). Soft never wiped body (Trap/Tech erase kill). */
    if (pg == TACK_PG_HOME) {
        /* 5.3 Home polish: style + FX scene + A/B + LOCK on one quiet row. */
        {
            char row[40];
            uint8_t locked = tack_is_locked();
            str_cpy(row, TACK_STYLE_NAME[tack_style_idx], sizeof row);
            tack_str_cat(row, " · ", sizeof row);
            tack_str_cat(row, TACK_FXSC_NAME[tack_fx_scene & 3u], sizeof row);
            tack_str_cat(row, tack_ab_live ? " · B" : " · A", sizeof row);
            draw_text_line(10, 22, 170, &AF_S, row, T_THEME, T_BG, 0);
            if (locked)
                draw_text_line(184, 22, 32, &AF_S, "LOCK", T_ACCENT, T_BG, 2);
            else {
                char octs[6];
                if (tack_octave == 0) { octs[0] = 'O'; octs[1] = '0'; octs[2] = 0; }
                else if (tack_octave > 0) { octs[0] = '+'; octs[1] = (char)('0' + tack_octave); octs[2] = 0; }
                else { octs[0] = '-'; octs[1] = (char)('0' + (-tack_octave)); octs[2] = 0; }
                draw_text_line(190, 22, 28, &AF_S, octs, tack_octave ? T_ACCENT : T_DIM, T_BG, 2);
            }
            if (tack_crec_mode) {
                char crec_ch[2] = { (char)(tack_crec_mode == 1u ? 'R' : 'P'), 0 };
                draw_text_line(222, 22, 14, &AF_S, crec_ch, T_ACCENT, T_BG, 0);
            }
        }

        /* Bigger now-playing: chord letter + quality + role + mode. */
        {
            uint8_t chord_bar = tack_hold ? tack_hold_bar : bar;
            const TackChord *nc = &tack_state.chords[chord_bar];
            const char *root = TACK_NOTES[nc->root % 12u];
            const char *role = tack_chord_tag(nc);
            char letter[2] = { root[0], 0 };
            draw_text_line(10, 36, 88, &AF_L, letter, T_THEME, T_BG, 0);
            str_cpy(b, root + 1u, sizeof b);
            if (b[0]) draw_text_line(10 + text_w(&AF_L, letter) - 3, 48, 40, &AF_M, b, T_THEME, T_BG, 0);
            str_cpy(b, tack_display_suffix(nc->quality), sizeof b);
            draw_text_line(92, 42, 70, &AF_M, b, T_ACCENT, T_BG, 0);
            draw_text_line(92, 58, 90, &AF_S, role, T_MID, T_BG, 0);
            draw_text_line(170, 42, 60, &AF_S, TACK_MODE_NAME[tack_mode_idx], T_DIM, T_BG, 2);
        }

        /* Four chord slots */
        for (i = 0; i < 4u; i++) {
            uint8_t pick_on = (i == (tack_pick_slot % TACK_SLOTS));
            uint8_t play_on = (tack_hold ? i == 0 : i == bar);
            uint16_t slot_col = i == 0u ? T_THEME : i == 1u ? T_ACCENT : i == 2u ? T_MID : T_RAISE;
            const TackChord *sc = &tack_state.chords[tack_hold ? tack_hold_bar : i];
            if (pick_on) {
                cv_rect(8 + (int32_t)i * 56, 74, 50, 15, T_SEL);
                cv_rect(8 + (int32_t)i * 56, 74, 3, 15, T_ACCENT);
            } else if (play_on) cv_rect(8 + (int32_t)i * 56, 74, 50, 2, slot_col);
            else cv_rect(8 + (int32_t)i * 56, 74, 50, 2, T_DIM);
            tack_chord_text(a, sizeof a, sc);
            draw_text_line(10 + i * 56u, 79, 52, &AF_S, a,
                           pick_on ? T_INK : T_TEXT, pick_on ? T_SEL : T_BG, 0);
        }
        cv_rect(8, 92, 224, 1, T_LINE);

        /* Moves — slightly tighter to free setlist strip. */
        {
            uint8_t prev = (uint8_t)((tack_pick_slot + TACK_SLOTS - 1u) % TACK_SLOTS);
            tack_state.focus = prev;
        }
        tack_next_moves(&tack_state, moves);
        for (i = 0; i < TACK_MOVES; i++) {
            y = 96u + i * 14u;
            uint8_t on = (i == tack_sel);
            const char *tag = tack_chord_tag(&moves[i]);
            uint16_t tag_col = on ? T_INK
                : (tag[0] == 'H' && tag[1] == 'O' ? T_ACCENT : T_MID);
            /* Always paint row bg — soft redraw leaves no SEL ghosts (5.7). */
            cv_rect(8, (int32_t)y - 1, 224, 13, on ? T_SEL : T_BG);
            if (on) cv_rect(8, (int32_t)y - 1, 3, 13, TACK_COL_SEL); /* strong pick rail */
            tack_chord_text(a, sizeof a, &moves[i]);
            draw_text_line(16, y, 16, &AF_S, (char[]){(char)('1' + (char)i), 0},
                           on ? T_INK : T_THEME, on ? T_SEL : T_BG, 0);
            draw_text_line(34, y, 100, &AF_M, a,
                           on ? T_INK : T_TEXT, on ? T_SEL : T_BG, 0);
            draw_text_line(140, y, 90, &AF_S, tag,
                           tag_col, on ? T_SEL : T_BG, 2);
        }

        /* 5.7 calm Home bottom: dirty-wipe then quiet A/B+FX, or MORE SET+hints ~2s. */
        lcd_fill(0, 178, 240, 62, T_BG); /* dirty-region only — no full blink */
        if (tack_arr_edit) {
            /* Spare A/B arrange strip while editing. */
            char ab[4];
            uint8_t alen = tack_arr_len ? tack_arr_len : 4u;
            if (alen > TACK_ARR_MAX) alen = TACK_ARR_MAX;
            ab[0] = (char)(tack_ab_live ? 'B' : 'A');
            ab[1] = 0;
            draw_text_line(10, 182, 18, &AF_M, ab, T_ACCENT, T_BG, 0);
            draw_text_line(28, 184, 36, &AF_S, tack_arr_on ? "ARR" : "off",
                           tack_arr_on ? T_MID : T_DIM, T_BG, 0);
            for (i = 0; i < (uint32_t)alen; i++) {
                int32_t x = 68 + (int32_t)i * ((alen <= 4u) ? 40 : 20);
                int32_t w = (alen <= 4u) ? 36 : 18;
                uint8_t is_b = tack_arr[i] ? 1u : 0u;
                uint8_t play_on = (tack_arr_on && song.playing && i == (tack_arr_pos % alen));
                uint8_t sel_on = (i == tack_arr_sel);
                uint16_t fill = sel_on ? ux_mix(T_BG, TACK_COL_SEL, 70)
                    : (play_on ? (is_b ? T_ACCENT : T_THEME)
                               : ux_mix(T_BG, is_b ? T_ACCENT : T_THEME, 36));
                uint16_t ink = sel_on ? T_INK : (play_on ? T_INK : tack_ab_col(is_b));
                cv_rect(x, 182, w, 14, fill);
                if (sel_on) cv_rect(x, 182, 2, 14, TACK_COL_SEL);
                {
                    char lab[2] = { (char)(is_b ? 'B' : 'A'), 0 };
                    draw_text_line((uint32_t)x + 4u, 184, (uint32_t)w - 4u, &AF_S, lab, ink, fill, 0);
                }
            }
            draw_text_line(10, 206, 220, &AF_S, "ARR · K1 col  K2 A/B  K3 len  K4 on", T_DIM, T_BG, 0);
        } else if (tack_home_show_more()) {
            /* MORE — SET strip + soft-key hints (SAVE held or ~2s after SAVE/LOCK/queue). */
            draw_text_line(10, 182, 40, &AF_S, "SET", T_MID, T_BG, 0);
            for (i = 0; i < TACK_PERF_N; i++) {
                int32_t x = 40 + (int32_t)i * 24;
                uint8_t sel_on = (i == tack_set_sel);
                uint8_t live_on = (i == tack_set_live);
                uint8_t q_on = (tack_q_perf == i);
                uint8_t used = tack_perf[i].used;
                uint16_t fill = sel_on ? ux_mix(T_BG, TACK_COL_SEL, 70)
                    : (live_on ? T_THEME
                               : (q_on ? T_ACCENT : ux_mix(T_BG, T_THEME, used ? 40 : 18)));
                uint16_t ink = (sel_on || live_on || q_on) ? T_INK : (used ? T_THEME : T_DIM);
                char lab[3];
                lab[0] = (char)('1' + i); lab[1] = 0;
                cv_rect(x, 180, 22, 16, fill);
                if (sel_on) cv_rect(x, 180, 2, 16, TACK_COL_SEL);
                if (q_on && !sel_on) cv_rect(x, 194, 22, 2, T_ACCENT);
                draw_text_line((uint32_t)x + 6u, 183, 14, &AF_S, lab, ink, fill, 0);
            }
            {
                char ab[4];
                ab[0] = (char)(tack_ab_live ? 'B' : 'A'); ab[1] = 0;
                draw_text_line(10, 202, 20, &AF_M, ab, tack_ab_col(tack_ab_live), T_BG, 0);
                draw_text_line(28, 204, 56, &AF_M, TACK_FXSC_NAME[tack_fx_scene & 3u], T_THEME, T_BG, 0);
                /* 6.14 Home chips: SAVE · SET · LOCK · GEN (MORE chrome) */
                {
                    uint16_t sf = ux_mix(T_BG, T_ACCENT, 48);
                    uint16_t tf = ux_mix(T_BG, T_THEME, 48);
                    uint16_t lf = tack_is_locked() ? ux_mix(T_BG, T_ACCENT, 70) : ux_mix(T_BG, T_DIM, 40);
                    uint16_t gf = ux_mix(T_BG, TACK_COL_GEN, 50);
                    cv_rect(88, 200, 36, 14, sf);
                    cv_rect(126, 200, 30, 14, tf);
                    cv_rect(158, 200, 36, 14, lf);
                    cv_rect(196, 200, 34, 14, gf);
                    draw_text_line(90, 202, 32, &AF_S, "SAVE", T_INK, sf, 0);
                    draw_text_line(128, 202, 26, &AF_S, "SET", T_INK, tf, 0);
                    draw_text_line(160, 202, 32, &AF_S, "LOCK", tack_is_locked() ? T_INK : T_DIM, lf, 0);
                    draw_text_line(198, 202, 30, &AF_S, "GEN", T_INK, gf, 0);
                }
            }
            if (tack_is_locked())
                draw_text_line(10, 216, 220, &AF_S, "LOCK  ·  SAVE·P  SET  A/B", T_DIM, T_BG, 0);
            else
                draw_text_line(10, 216, 220, &AF_S, "SAVE·P  SET  LOCK  ENV·GEN", T_DIM, T_BG, 0);
        } else {
            /* Quiet play surface — 6.14 SAVE/SET/LOCK/GEN chips; A/B + FX; ARP when on. */
            {
                char ab[4];
                ab[0] = (char)(tack_ab_live ? 'B' : 'A'); ab[1] = 0;
                draw_text_line(10, 200, 24, &AF_L, ab, tack_ab_col(tack_ab_live), T_BG, 0);
                draw_text_line(40, 206, 56, &AF_M, TACK_FXSC_NAME[tack_fx_scene & 3u], T_THEME, T_BG, 0);
                {
                    uint16_t sf = ux_mix(T_BG, T_ACCENT, 36);
                    uint16_t tf = ux_mix(T_BG, T_THEME, 36);
                    uint16_t lf = tack_is_locked() ? ux_mix(T_BG, T_ACCENT, 64) : ux_mix(T_BG, T_DIM, 28);
                    uint16_t gf = ux_mix(T_BG, TACK_COL_GEN, 36);
                    cv_rect(96, 204, 32, 12, sf);
                    cv_rect(130, 204, 28, 12, tf);
                    cv_rect(160, 204, 32, 12, lf);
                    cv_rect(194, 204, 30, 12, gf);
                    draw_text_line(98, 205, 28, &AF_S, "SAVE", T_DIM, sf, 0);
                    draw_text_line(132, 205, 24, &AF_S, "SET", T_DIM, tf, 0);
                    draw_text_line(162, 205, 28, &AF_S, "LOCK",
                                   tack_is_locked() ? T_ACCENT : T_DIM, lf, 0);
                    draw_text_line(196, 205, 26, &AF_S, "GEN", T_DIM, gf, 0);
                }
                if (tack_arp_is_on()) {
                    uint16_t af = ux_mix(T_BG, TACK_COL_SEL, 48);
                    cv_rect(96, 188, 40, 12, af);
                    draw_text_line(100, 189, 34, &AF_S, "ARP", T_INK, af, 0);
                }
            }
        }
        return;
    }

    /* ---- edit pages share a calm list layout ---- */
    /* 6.27 Soft: LIBRARY + STYLE — no HOME-back tip (title is enough); other pages keep it. */
    if (pg != TACK_PG_SOUNDS && pg != TACK_PG_STYLE) {
        draw_text_line(10, 24, 220, &AF_S,
                       tack_is_locked() ? "LOCKED — HOME / transport only" : "HOME back to play",
                       tack_is_locked() ? T_ACCENT : T_DIM, T_BG, 0);
        cv_rect(8, 38, 224, 1, tack_is_locked() ? T_ACCENT : T_DIM);
    }

    if (pg == TACK_PG_SOUNDS) {
        /* 6.27 Soft library: AF_M rows / no jump / no fat LIVE / one tip / dirty blit. */
        static uint8_t last_snd_depth = 0xFFu;
        static uint8_t last_live_bank = 0xFFu;
        static uint8_t last_snd_cur = 0xFFu;
        static uint8_t last_lib_top = 0xFFu;
        static uint8_t last_lib_foot = 0xFFu;
        static uint8_t last_pick = 0xFFu;
        static uint8_t last_top = 0xFFu;
        static uint8_t last_live_pk = 0xFFu;
        static uint8_t last_trk = 0xFFu;
        static uint8_t last_foot_pick = 0xFFu;
        static uint8_t last_pre_bank = 0xFFu;
        uint8_t depth_flip = (last_snd_depth != tack_snd_depth) ? 1u : 0u;
        if (need_full || depth_flip) {
            lcd_fill(0, 28, 240, 212, T_BG);
            last_live_bank = 0xFFu;
            last_snd_cur = 0xFFu;
            last_lib_top = 0xFFu;
            last_lib_foot = 0xFFu;
            last_pick = 0xFFu;
            last_top = 0xFFu;
            last_live_pk = 0xFFu;
            last_trk = 0xFFu;
            last_foot_pick = 0xFFu;
            last_pre_bank = 0xFFu;
        }
        last_snd_depth = tack_snd_depth;
        if (tack_snd_depth == 0u) {
            /* BANKS — windowed STYLE craft */
            uint8_t live_bank = (uint8_t)(tack_snd_bank % TACK_LIB_N);
            uint8_t top = tack_list_top(tack_snd_cursor, TACK_LIB_N);
            uint8_t shown = (TACK_LIB_N < TACK_LIST_VIS) ? TACK_LIB_N : TACK_LIST_VIS;
            uint8_t si;
            if (need_full || depth_flip || last_lib_top == 0xFFu || last_lib_top != top) {
                lcd_fill(0, TACK_LIST_Y, 240,
                         (uint32_t)TACK_LIST_VIS * TACK_ROW_H, T_BG);
                for (si = 0; si < shown; si++)
                    tack_paint_snd_lib_row((uint8_t)(top + si), si);
            } else {
                if (last_live_bank != live_bank) {
                    if (last_live_bank < TACK_LIB_N && last_live_bank >= top
                        && last_live_bank < (uint8_t)(top + shown))
                        tack_paint_snd_lib_row(last_live_bank, (uint8_t)(last_live_bank - top));
                    if (live_bank >= top && live_bank < (uint8_t)(top + shown))
                        tack_paint_snd_lib_row(live_bank, (uint8_t)(live_bank - top));
                }
                if (last_snd_cur != tack_snd_cursor) {
                    if (last_snd_cur < TACK_LIB_N && last_snd_cur >= top
                        && last_snd_cur < (uint8_t)(top + shown)
                        && last_snd_cur != last_live_bank && last_snd_cur != live_bank)
                        tack_paint_snd_lib_row(last_snd_cur, (uint8_t)(last_snd_cur - top));
                    if (tack_snd_cursor >= top && tack_snd_cursor < (uint8_t)(top + shown)
                        && tack_snd_cursor != last_live_bank && tack_snd_cursor != live_bank)
                        tack_paint_snd_lib_row(tack_snd_cursor, (uint8_t)(tack_snd_cursor - top));
                }
            }
            last_live_bank = live_bank;
            last_snd_cur = tack_snd_cursor;
            last_lib_top = top;
            if (need_full || depth_flip || last_lib_foot != live_bank) {
                uint16_t col = TACK_BANK_COL[live_bank];
                lcd_fill(0, 186, 240, 40, T_BG);
                draw_text_line(10, 192, 220, &AF_M, TACK_BANK_NAME[live_bank], col, T_BG, 0);
                last_lib_foot = live_bank;
            }
        } else {
            /* PRESETS — AF_M window; one footer line */
            uint8_t n = tack_lib_sound_n(tack_snd_bank);
            uint8_t bank = (uint8_t)(tack_snd_bank % TACK_LIB_N);
            uint8_t top, shown, si;
            uint8_t live_pk = tack_snd_live_pick();
            uint8_t trk = (uint8_t)(tack_mute_sel & 3u);
            uint8_t bank_flip = (last_pre_bank != bank) ? 1u : 0u;
            top = tack_snd_list_top(n);
            shown = (n < TACK_SND_VIS) ? n : TACK_SND_VIS;
            if (need_full || depth_flip || bank_flip || last_top == 0xFFu || last_top != top) {
                lcd_fill(0, TACK_SND_LIST_Y, 240,
                         (uint32_t)TACK_SND_VIS * TACK_SND_ROW_H, T_BG);
                for (si = 0; si < shown; si++)
                    tack_paint_snd_preset_row((uint8_t)(top + si), si);
            } else {
                if (last_pick != tack_snd_pick) {
                    if (last_pick >= top && last_pick < (uint8_t)(top + shown))
                        tack_paint_snd_preset_row(last_pick, (uint8_t)(last_pick - top));
                    if (tack_snd_pick >= top && tack_snd_pick < (uint8_t)(top + shown))
                        tack_paint_snd_preset_row(tack_snd_pick, (uint8_t)(tack_snd_pick - top));
                }
                if (last_live_pk != live_pk) {
                    if (last_live_pk < n && last_live_pk >= top && last_live_pk < (uint8_t)(top + shown)
                        && last_live_pk != last_pick && last_live_pk != tack_snd_pick)
                        tack_paint_snd_preset_row(last_live_pk, (uint8_t)(last_live_pk - top));
                    if (live_pk < n && live_pk >= top && live_pk < (uint8_t)(top + shown)
                        && live_pk != last_pick && live_pk != tack_snd_pick)
                        tack_paint_snd_preset_row(live_pk, (uint8_t)(live_pk - top));
                }
            }
            last_pick = tack_snd_pick;
            last_top = top;
            last_live_pk = live_pk;
            last_pre_bank = bank;
            if (need_full || depth_flip || last_trk != trk || last_foot_pick != tack_snd_pick) {
                char foot[40];
                lcd_fill(0, 186, 240, 40, T_BG);
                str_cpy(foot, TACK_LANE[trk], sizeof foot);
                tack_str_cat(foot, " · ", sizeof foot);
                tack_str_cat(foot, tack_lib_sound_name(bank, tack_snd_pick), sizeof foot);
                draw_text_line(10, 192, 220, &AF_M, foot, T_THEME, T_BG, 0);
                last_trk = trk;
                last_foot_pick = tack_snd_pick;
            }
        }
        return;
    }

    if (pg == TACK_PG_STYLE) {
        /* 6.27 Soft STYLE = library craft: AF_M windowed, no fat LIVE, one footer, dirty blit. */
        static uint8_t last_live_sty = 0xFFu;
        static uint8_t last_sty_cur = 0xFFu;
        static uint8_t last_sty_top = 0xFFu;
        static int16_t last_sty_bpm = -1;
        static uint8_t last_sty_dens = 0xFFu;
        uint8_t top = tack_list_top(tack_style_cursor, TACK_STYLE_COUNT);
        uint8_t shown = (TACK_STYLE_COUNT < TACK_LIST_VIS) ? TACK_STYLE_COUNT : TACK_LIST_VIS;
        uint8_t si;
        uint8_t foot_dirty = (need_full
                              || last_sty_bpm != song.g[G_BPM]
                              || last_sty_dens != tack_dens) ? 1u : 0u;
        if (need_full || last_sty_top == 0xFFu || last_sty_top != top) {
            lcd_fill(0, TACK_LIST_Y, 240,
                     (uint32_t)TACK_LIST_VIS * TACK_ROW_H, T_BG);
            for (si = 0; si < shown; si++)
                tack_paint_style_row((uint8_t)(top + si), si);
        } else {
            if (last_live_sty != tack_style_idx) {
                if (last_live_sty < TACK_STYLE_COUNT && last_live_sty >= top
                    && last_live_sty < (uint8_t)(top + shown))
                    tack_paint_style_row(last_live_sty, (uint8_t)(last_live_sty - top));
                if (tack_style_idx >= top && tack_style_idx < (uint8_t)(top + shown))
                    tack_paint_style_row(tack_style_idx, (uint8_t)(tack_style_idx - top));
            }
            if (last_sty_cur != tack_style_cursor) {
                if (last_sty_cur < TACK_STYLE_COUNT && last_sty_cur >= top
                    && last_sty_cur < (uint8_t)(top + shown)
                    && last_sty_cur != last_live_sty && last_sty_cur != tack_style_idx)
                    tack_paint_style_row(last_sty_cur, (uint8_t)(last_sty_cur - top));
                if (tack_style_cursor >= top && tack_style_cursor < (uint8_t)(top + shown)
                    && tack_style_cursor != last_live_sty && tack_style_cursor != tack_style_idx)
                    tack_paint_style_row(tack_style_cursor, (uint8_t)(tack_style_cursor - top));
            }
        }
        last_live_sty = tack_style_idx;
        last_sty_cur = tack_style_cursor;
        last_sty_top = top;
        if (foot_dirty) {
            lcd_fill(0, 186, 240, 40, T_BG);
            str_cpy(a, "BPM ", sizeof a);
            tack_str_cat(a, bpm, sizeof a);
            tack_str_cat(a, " · dens ", sizeof a);
            fmt_int(b, tack_dens);
            tack_str_cat(a, b, sizeof a);
            draw_text_line(10, 192, 220, &AF_M, a, T_THEME, T_BG, 0);
            last_sty_bpm = song.g[G_BPM];
            last_sty_dens = tack_dens;
        }
        return;
    }

    if (pg == TACK_PG_ALGO) {
        /* 6.1 LFO: clear Track → Sound (active library) */
        uint16_t col = TACK_BANK_COL[tack_snd_bank % TACK_LIB_N];
        char lib[28];
        str_cpy(lib, "lib ", sizeof lib);
        tack_str_cat(lib, TACK_BANK_NAME[tack_snd_bank % TACK_LIB_N], sizeof lib);
        draw_text_line(10, 44, 220, &AF_S, "K1 track  K2/K3 sound  K4→PRESET", T_DIM, T_BG, 0);
        draw_text_line(10, 58, 220, &AF_S, lib, col, T_BG, 0);
        for (i = 0; i < 4u; i++) {
            y = 76u + i * 26u;
            uint8_t on = (i == (tack_mute_sel & 3u));
            uint16_t fill = on ? T_SEL : T_BG;
            cv_rect(8, (int32_t)y - 2, 224, 24, fill); /* 6.12 soft */
            draw_text_line(14, y, 56, &AF_M, TACK_LANE[i],
                           on ? T_INK : T_DIM, fill, 0);
            draw_text_line(74, y, 150, &AF_S,
                           tack_trk_lib_name(i),
                           on ? T_INK : T_TEXT, fill, 0);
        }
        /* Big TRACK + SOUND — catalog name for focused lane */
        {
            char big[36];
            str_cpy(big, TACK_LANE[tack_mute_sel & 3u], sizeof big);
            tack_str_cat(big, " · ", sizeof big);
            tack_str_cat(big, tack_trk_lib_name(tack_mute_sel & 3u), sizeof big);
            draw_text_line(10, 186, 220, &AF_M, big, col, T_BG, 0);
        }
        draw_text_line(10, 208, 220, &AF_S, "PRESETS · library sounds per track", T_DIM, T_BG, 0);
        return;
    }
    if (pg == TACK_PG_PRESET) {
        /* 6.3: library presets per track — not engine factory banks */
        {
            char hdr[36];
            uint16_t col = TACK_BANK_COL[tack_snd_bank % TACK_LIB_N];
            str_cpy(hdr, TACK_BANK_NAME[tack_snd_bank % TACK_LIB_N], sizeof hdr);
            tack_str_cat(hdr, " presets", sizeof hdr);
            draw_text_line(10, 44, 220, &AF_S, hdr, col, T_BG, 0);
            draw_text_line(10, 58, 220, &AF_S, "K1 track  K2/K3 lib preset  K4 library", T_DIM, T_BG, 0);
        }
        for (i = 0; i < 4u; i++) {
            y = 76u + i * 26u;
            uint8_t on = (i == (tack_mute_sel & 3u));
            uint16_t fill = on ? T_SEL : T_BG;
            cv_rect(8, (int32_t)y - 2, 224, 24, fill); /* 6.12 soft */
            draw_text_line(14, y, 50, &AF_M, TACK_LANE[i],
                           on ? T_INK : T_DIM, fill, 0);
            draw_text_line(70, y, 150, &AF_S,
                           tack_trk_lib_name(i),
                           on ? T_INK : T_TEXT, fill, 0);
            draw_text_line(70, y + 12, 160, &AF_S,
                           on ? "lib preset on this track" : "",
                           on ? T_INK : T_DIM, fill, 0);
        }
        draw_text_line(10, 200, 220, &AF_S, "library presets · mode + kit stay", T_DIM, T_BG, 0);
        return;
    }

    if (pg == TACK_PG_FX) {
        /* 6.24 calm FX Room: scene chips + 7-row window + dirty blit (no tip stack). */
        static uint8_t last_fx_sel = 0xFFu;
        static uint8_t last_fx_top = 0xFFu;
        static uint8_t last_fx_scene = 0xFFu;
        static uint8_t last_fx_pong = 0xFFu;
        static uint8_t last_fx_q = 0xFFu;
        uint8_t top = tack_fx_list_top();
        uint8_t scene_dirty = (need_full || last_fx_scene != tack_fx_scene
                               || last_fx_q != tack_q_fxsc) ? 1u : 0u;
        uint8_t list_dirty = (need_full || last_fx_top == 0xFFu || last_fx_top != top
                              || last_fx_sel == 0xFFu) ? 1u : 0u;
        uint8_t foot_dirty = (need_full || last_fx_pong != tack_pong
                              || last_fx_scene != tack_fx_scene
                              || last_fx_sel != tack_fx_sel) ? 1u : 0u;
        uint8_t si2;
        if (need_full) lcd_fill(0, 36, 240, 204, T_BG); /* page enter only */
        if (scene_dirty) tack_paint_fx_scene_strip();
        if (list_dirty) {
            /* window scroll or enter — soft wipe list band only, then paint visible */
            if (!need_full)
                lcd_fill(0, TACK_FX_LIST_Y, 240, (uint32_t)TACK_FX_VIS * TACK_FX_ROW_H, T_BG);
            for (si2 = 0; si2 < TACK_FX_VIS; si2++)
                tack_paint_fx_row((uint8_t)(top + si2), si2);
        } else if (last_fx_sel != tack_fx_sel) {
            /* cursor move inside same window — dirty old + new only */
            if (last_fx_sel >= top && last_fx_sel < top + TACK_FX_VIS)
                tack_paint_fx_row(last_fx_sel, (uint8_t)(last_fx_sel - top));
            if (tack_fx_sel >= top && tack_fx_sel < top + TACK_FX_VIS)
                tack_paint_fx_row(tack_fx_sel, (uint8_t)(tack_fx_sel - top));
        } else {
            /* value / freeze change — repaint focused row */
            if (tack_fx_sel >= top && tack_fx_sel < top + TACK_FX_VIS)
                tack_paint_fx_row(tack_fx_sel, (uint8_t)(tack_fx_sel - top));
        }
        /* scene apply changes many values — when scene dirty, refresh visible values */
        if (scene_dirty && !list_dirty && !need_full) {
            for (si2 = 0; si2 < TACK_FX_VIS; si2++)
                tack_paint_fx_row((uint8_t)(top + si2), si2);
        }
        last_fx_sel = tack_fx_sel;
        last_fx_top = top;
        last_fx_scene = tack_fx_scene;
        last_fx_q = tack_q_fxsc;
        if (foot_dirty) {
            lcd_fill(0, 176, 240, 48, T_BG);
            {
                char big[36];
                str_cpy(big, TACK_FXROW[tack_fx_sel % TACK_FX_N], sizeof big);
                tack_str_cat(big, "  ", sizeof big);
                {
                    char vb[8]; int16_t val = 0; uint8_t txt = 0;
                    tack_fx_row_value(tack_fx_sel, vb, sizeof vb, &val, &txt);
                    if (txt) tack_str_cat(big, vb, sizeof big);
                    else { char n[8]; fmt_int(n, val); tack_str_cat(big, n, sizeof big); }
                }
                draw_text_line(10, 180, 140, &AF_M, big, T_THEME, T_BG, 0);
                draw_text_line(150, 182, 80, &AF_S, tack_pong ? "PONG ON" : "PONG",
                               tack_pong ? TACK_COL_LIVE : T_DIM, T_BG, 2);
            }
            draw_text_line(10, 204, 220, &AF_S, "K1 row  K2 set  K3 pong  K4 scene", T_DIM, T_BG, 0);
            last_fx_pong = tack_pong;
        }
        return;
    }

    if (pg == TACK_PG_DRUMS) {
        static const char *const DRM_ROW[10] = {
            "KIT", "DENSITY", "HATS", "SPARE", "KICK LOCK", "FILL", "RATCHET",
            "CRUSH", "COMP", "GEN"
        };
        static const char *const HAT_N[3] = {"QUIET", "NORM", "MORE"};
        static const char *const RAT_N[4] = {"OFF", "x2", "x3", "x4"};
        static const uint16_t PAD_COL[8] = {
            RGB(255, 88, 72), RGB(240, 168, 36), RGB(255, 200, 48), RGB(80, 200, 140),
            RGB(64, 210, 120), RGB(80, 140, 255), RGB(220, 120, 255), RGB(120, 200, 255)
        };
        /* 6.12: body wipe only on page enter / depth flip — soft PART cursor overdraws rows. */
        {
            static uint8_t last_drm_depth = 0xFFu;
            if (need_full || last_drm_depth != tack_drm_depth)
                lcd_fill(0, 36, 240, 204, T_BG);
            last_drm_depth = tack_drm_depth;
        }
        /* depth tabs — PART | FEEL (lcd chips + text blit) */
        {
            uint16_t pf = (!tack_drm_depth) ? ux_mix(T_BG, TACK_COL_SEL, 58) : ux_mix(T_BG, T_THEME, 12);
            uint16_t ff = tack_drm_depth ? ux_mix(T_BG, TACK_COL_SEL, 58) : ux_mix(T_BG, T_THEME, 12);
            lcd_fill(8, 36, 70, 16, pf);
            lcd_fill(82, 36, 70, 16, ff);
            draw_text_line(12, 38, 62, &AF_S, "PART", !tack_drm_depth ? T_INK : T_DIM, pf, 0);
            draw_text_line(86, 38, 62, &AF_S, "FEEL", tack_drm_depth ? T_INK : T_DIM, ff, 0);
            draw_text_line(160, 38, 70, &AF_S, tack_kit_label(&trk[3]), T_THEME, T_BG, 2);
        }
        if (tack_drm_depth == 0u) {
            /* 6.10 PART: per-row canvas blit so ○/● actually hit the 240×240 panel.
             * Root cause in 6.9: tack_drm_circ/cv_rrect drew into the last draw_text_line
             * strip (font-tall ~15px) and never cv_blit'd — grid was blank. */
            uint8_t bs = tack_bar_steps();
            uint8_t lane = tack_drm_lane & 7u;
            uint8_t cur = tack_drm_step;
            uint8_t play_s = 0;
            uint8_t nsteps = bs > 16u ? 16u : bs;
            const int32_t row_h = 18;
            if (cur >= bs) cur = 0;
            if (song.playing)
                play_s = tack_seq_step16();
            draw_text_line(10, 54, 140, &AF_S, "DRUM PART", T_THEME, T_BG, 0);
            draw_text_line(150, 54, 80, &AF_S, "K4 GEN", T_DIM, T_BG, 2);
            for (i = 0; i < 8u; i++) {
                uint8_t row_on = (i == lane);
                uint16_t colp = PAD_COL[i];
                int32_t yy = 70 + (int32_t)i * row_h;
                uint16_t lab_ink = row_on ? T_INK : T_TEXT;
                uint16_t lab_bg = row_on ? ux_mix(T_BG, colp, 40) : T_BG;
                uint8_t si;
                const char *nm = tack_drm_lane_name(i);
                uint8_t locked = tack_drm_lane_locked((uint8_t)i);
                /* one strip per voice row — circles live here, then blit to screen */
                cv_begin(240u, (uint32_t)row_h, lab_bg);
                if (row_on)
                    cv_rect(0, 0, 3, row_h, colp);
                if (locked)
                    cv_text_on(6, 3, &AF_S, "L", T_DIM, lab_bg);
                cv_text_on(locked ? 14 : 6, 3, &AF_S, nm, locked ? T_DIM : lab_ink, lab_bg);
                for (si = 0; si < nsteps; si++) {
                    int32_t cx = 50 + (int32_t)si * 11 + 5;
                    int32_t cy = row_h / 2;
                    uint8_t on = tack_drm_step_on((uint8_t)i, si);
                    uint8_t sel = (row_on && si == cur);
                    uint8_t ph = (song.playing && si == play_s);
                    uint16_t ink = on ? colp : T_DIM;
                    int32_t rr = sel ? 5 : 4;
                    if (sel && !on) ink = TACK_COL_SEL;
                    tack_drm_circ(cx, cy, rr, ink, lab_bg, on);
                    if (sel) {
                        /* calm cursor box — no blink */
                        cv_frame(cx - rr - 2, cy - rr - 2, (rr * 2 + 1) + 4, (rr * 2 + 1) + 4, TACK_COL_SEL);
                    }
                    if (ph)
                        cv_rect(cx - 4, cy + rr + 1, 9, 1, T_ACCENT);
                }
                cv_blit(0, (uint32_t)yy);
            }
            draw_text_line(10, 218, 220, &AF_S, "K1 move  K2 hit  K3 lane  K4 GEN", T_DIM, T_BG, 0);
            return;
        }
        /* FEEL depth — dens / hats / bus (kit also here) */
        draw_text_line(10, 44, 220, &AF_S, "K1 row  K2 fire  K3± dens/crush/comp", T_DIM, T_BG, 0);
        for (i = 0; i < 10u; i++) {
            y = 58u + i * 13u;
            uint8_t on = (i == tack_drm_sel);
            uint16_t col = TACK_DRM_COL[i];
            uint16_t fill = on ? ux_mix(T_BG, col, 58) : T_BG;
            cv_rect(8, (int32_t)y - 2, 224, 12, fill); /* 6.24 soft */
            cv_rect(8, (int32_t)y - 2, 4, 12, col);
            draw_text_line(16, y, 120, &AF_S, DRM_ROW[i],
                           on ? T_INK : T_TEXT, fill, 0);
            b[0] = 0;
            if (i == 0) str_cpy(b, tack_kit_label(&trk[3]), sizeof b);
            else if (i == 1) fmt_int(b, tack_dens);
            else if (i == 2) str_cpy(b, HAT_N[tack_drm_hats % 3u], sizeof b);
            else if (i == 3) str_cpy(b, tack_drm_spare ? "ON" : "OFF", sizeof b);
            else if (i == 4) str_cpy(b, tack_kick_lock ? "ON" : "OFF", sizeof b);
            else if (i == 5) str_cpy(b, tack_fill ? "ON" : "OFF", sizeof b);
            else if (i == 6) str_cpy(b, RAT_N[tack_drm_ratchet & 3u], sizeof b);
            else if (i == 7) fmt_int(b, tack_drm_dist);
            else if (i == 8) fmt_int(b, tack_drm_comp);
            else str_cpy(b, tack_rand_drm ? "GO" : "OFF", sizeof b);
            draw_text_line(150, y, 70, &AF_S, b,
                           on ? T_INK : col, fill, 2);
        }
        draw_text_line(10, 200, 220, &AF_S, "K4 PART · kit @BAR · GLO nav", T_DIM, T_BG, 0);
        return;
    }

    if (pg == TACK_PG_FEEL) {
        static const char *const FEEL[12] = {
            "DENSITY", "FILL", "CHAOS", "ROLL/RAND", "UNDO", "AUTO", "CLK", "LVL", "PAN",
            "ARP", "MUTE", "SOLO"
        };
        static const char *const CLK_N[3] = {"INT", "USB", "TRS"};
        static const char *const ARP_N[7] = {"OFF", "UP", "DN", "UPDN", "RND", "ORD", "REPEAT"};
        uint8_t clk = (uint8_t)song.g[G_CLOCK];
        uint8_t am;
        if (clk > 2u) clk = 0;
        am = (uint8_t)clamp((int32_t)trk[tack_mute_sel & 3u].p[P_AMODE], 0, 6);
        draw_text_line(10, 36, 220, &AF_S, "K1 row  K2 fire  K3 set", T_DIM, T_BG, 0);
        for (i = 0; i < 12u; i++) {
            y = 48u + i * 13u;
            uint8_t on = (i == tack_feel_sel);
            uint16_t fill = on ? ux_mix(T_BG, TACK_COL_LIVE, 58) : T_BG;
            cv_rect(8, (int32_t)y - 1, 224, 12, fill); /* 6.24 soft — no SEL ghost */
            if (on) cv_rect(8, (int32_t)y - 1, 3, 12, TACK_COL_LIVE);
            draw_text_line(14, y, 100, &AF_S, FEEL[i],
                           on ? T_INK : T_TEXT, fill, 0);
            b[0] = 0;
            if (i == 0) fmt_int(b, tack_dens);
            else if (i == 1) str_cpy(b, tack_fill ? "ON" : "OFF", sizeof b);
            else if (i == 2) str_cpy(b, tack_chaos ? "ON" : "OFF", sizeof b);
            else if (i == 4) fmt_int(b, tack_undo_n);
            else if (i == 5) str_cpy(b, tack_auto ? "ON" : "OFF", sizeof b);
            else if (i == 6) str_cpy(b, CLK_N[clk], sizeof b);
            else if (i == 7) {
                fmt_int(b, tack_level[tack_mute_sel & 3u]);
            } else if (i == 8) {
                int8_t pv = tack_lane_pan[tack_mute_sel & 3u];
                if (pv < 0) { b[0] = '-'; fmt_int(b + 1, (int16_t)(-pv)); }
                else fmt_int(b, (int16_t)pv);
            } else if (i == 9) {
                str_cpy(b, ARP_N[am], sizeof b);
                tack_str_cat(b, " ", sizeof b);
                tack_str_cat(b, TACK_LANE[tack_mute_sel & 3u], sizeof b);
            } else if (i == 10) {
                str_cpy(b, TACK_LANE[tack_mute_sel & 3u], sizeof b);
                if (tack_mute & (1u << (tack_mute_sel & 3u)))
                    tack_str_cat(b, " M", sizeof b);
            } else if (i == 11) {
                str_cpy(b, TACK_LANE[tack_mute_sel & 3u], sizeof b);
                if (tack_solo & (1u << (tack_mute_sel & 3u)))
                    tack_str_cat(b, " S", sizeof b);
            } else str_cpy(b, "GO", sizeof b);
            draw_text_line(150, y, 70, &AF_S, b,
                           on ? T_INK : T_DIM, fill, 2);
        }
        draw_text_line(10, 210, 220, &AF_S, "ARP K2/K3 · MUTE/SOLO LOCK-ok", T_DIM, T_BG, 0);
        return;
    }

    if (pg == TACK_PG_LOOP) {
        /* RC4: five aligned controls and readable, ASCII-only LOOP guidance. */
        static const char *const LOOP_ROW[5] = {
            "REC", "LOOP", "QUANTIZE", "RHYTHM", "EXPORT"
        };
        {
            char st[32];
            str_cpy(st, TACK_STYLE_NAME[tack_style_idx], sizeof st);
            tack_str_cat(st, " / ", sizeof st);
            if (tack_crec_mode == 1u) tack_str_cat(st, "REC", sizeof st);
            else if (tack_crec_mode == 2u) tack_str_cat(st, "LOOP", sizeof st);
            else if (tack_crec_len) tack_str_cat(st, "HELD", sizeof st);
            else tack_str_cat(st, "IDLE", sizeof st);
            if (tack_crec_len) {
                char n1[8], n2[8];
                tack_str_cat(st, " ", sizeof st);
                fmt_int(n2, (tack_crec_pos % tack_crec_len) + 1u);
                fmt_int(n1, tack_crec_len);
                tack_str_cat(st, n2, sizeof st);
                tack_str_cat(st, "/", sizeof st);
                tack_str_cat(st, n1, sizeof st);
            }
            draw_text_line(10, 42, 160, &AF_S, st, T_THEME, T_BG, 0);
            if (tack_crec_qnt)
                draw_text_line(180, 42, 50, &AF_S, "QNT", T_ACCENT, T_BG, 2);
        }
        /* 5.9 USER bank status */
        {
            char ub[40];
            if (TACK_USER_PHRASE_COUNT) {
                const tack_user_phrase_t *up = &TACK_USER_PHRASE[tack_user_phrase_sel % TACK_USER_PHRASE_COUNT];
                str_cpy(ub, "USER: ", sizeof ub);
                tack_str_cat(ub, up->name[0] ? up->name : "PHRASE", sizeof ub);
                draw_text_line(10, 26, 220, &AF_S, ub, TACK_COL_LEARN, T_BG, 0);
            } else {
                draw_text_line(10, 26, 220, &AF_S, "USER: EMPTY", T_DIM, T_BG, 0);
            }
        }
        for (i = 0; i < 5u; i++) {
            y = 64u + i * 22u;
            uint8_t on = (i == tack_loop_sel);
            uint16_t accent = tack_loop_row_col((uint8_t)(i < 4u ? i : 0u));
            /* Selected row tint = mix(BG, row accent) — distinct Rec/Qnt/Rhythm/Export */
            uint16_t row_bg = on ? ux_mix(T_BG, accent, 55) : T_BG;
            uint16_t row_ink = on ? T_INK : T_TEXT;
            cv_rect(8, (int32_t)y - 2, 224, 22, row_bg); /* always paint — no soft ghost */
            if (on) cv_rect(8, (int32_t)y - 2, 4, 22, accent);
            draw_text_line(16, y + 1u, 110, &AF_M, LOOP_ROW[i], row_ink, row_bg, 0);
            b[0] = 0;
            if (i == 0) {
                if (tack_crec_mode == 1u) {
                    /* MIDI events are finalized at stop: do not imply a live count. */
                    str_cpy(b, "ARMED", sizeof b);
                } else if (tack_crec_len) str_cpy(b, "OK", sizeof b);
                else str_cpy(b, "OFF", sizeof b);
            } else if (i == 1) {
                str_cpy(b, tack_crec_mode == 2u ? "ON" : "OFF", sizeof b);
            } else if (i == 2) str_cpy(b, tack_crec_qnt ? "ON" : "OFF", sizeof b);
            else if (i == 3) str_cpy(b, TACK_RHY_NAME[tack_crec_rhy % 7u], sizeof b);
            else str_cpy(b, tack_ex_armed ? "READY" : "ARM", sizeof b);
            {
                uint16_t vcol = row_ink;
                if (i == 0 && (tack_crec_mode == 1u || tack_crec_len)) vcol = on ? T_INK : TACK_COL_REC;
                if (i == 1 && tack_crec_mode == 2u) vcol = on ? T_INK : TACK_COL_LP;
                if (i == 2 && tack_crec_qnt) vcol = on ? T_INK : TACK_COL_QNT;
                if (i == 3 && tack_crec_rhy) vcol = on ? T_INK : TACK_COL_RHY;
                draw_text_line(130, y + 1u, 90, &AF_M, b, vcol, row_bg, 2);
            }
        }
        /* Four readable chips; follow the existing phrase cursor, no new controls. */
        lcd_fill(0, 176, 240, 64, T_BG);
        cv_rect(8, 178, 224, 1, T_LINE);
        if (!tack_crec_len) {
            draw_text_line(10, 184, 220, &AF_S, "REC then LOOP or APPLY", T_DIM, T_BG, 0);
        } else {
            uint8_t first = (uint8_t)(((tack_crec_pos % tack_crec_len) / 4u) * 4u);
            uint8_t show = (uint8_t)(tack_crec_len - first);
            if (show > 4u) show = 4u;
            for (i = 0; i < show; i++) {
                int32_t x = 10 + (int32_t)i * 56;
                uint8_t idx = (uint8_t)(first + i);
                uint8_t live = (tack_crec_mode == 2u && idx == (tack_crec_pos % tack_crec_len));
                uint16_t fill = live ? T_THEME : ux_mix(T_BG, T_THEME, 28);
                char lab[12];
                tack_chord_text(lab, sizeof lab, &tack_crec[idx]);
                cv_rect(x, 183, 52, 18, fill);
                draw_text_line((uint32_t)x + 2u, 184, 48, &AF_S, lab,
                               live ? T_INK : T_TEXT, fill, 0);
            }
        }
        draw_text_line(10, 205, 220, &AF_S, "K1 ROW  K2 ACT  K4 APPLY", T_DIM, T_BG, 0);
        draw_text_line(10, 222, 220, &AF_S,
                       tack_ex_armed ? "EXPORT: HARMONY ONLY" : "MIDI notes / keys 1-6 moves",
                       tack_ex_armed ? T_ACCENT : T_DIM, T_BG, 0);
        return;
    }

    if (pg == TACK_PG_LEARN) {
        /* 5.9 MIDI LEARN — mint accent, soft list */
        draw_text_line(10, 40, 220, &AF_S, "K1 target  K2 arm  K3 clr  K4 save", T_DIM, T_BG, 0);
        {
            uint8_t ti;
            for (ti = 0; ti < 8u; ti++) {
                y = 56u + ti * 14u;
                uint8_t on = (ti == (tack_learn_target % 8u));
                uint16_t fill = on ? ux_mix(T_BG, TACK_COL_LEARN, 55) : T_BG;
                uint16_t ink = on ? T_INK : T_TEXT;
                char line[28];
                uint8_t cc = tack_learn_map[ti];
                cv_rect(8, (int32_t)y - 1, 224, 14, fill); /* 6.24 soft */
                if (on) cv_rect(8, (int32_t)y - 1, 3, 14, TACK_COL_LEARN);
                str_cpy(line, TACK_LEARN_TGT[ti], sizeof line);
                if (cc == 0xFFu) tack_str_cat(line, "  —", sizeof line);
                else {
                    char n[8];
                    tack_str_cat(line, "  CC", sizeof line);
                    fmt_int(n, cc);
                    tack_str_cat(line, n, sizeof line);
                }
                draw_text_line(16, y, 160, &AF_S, line, ink, fill, 0);
            }
        }
        draw_text_line(10, 176, 220, &AF_S,
                       tack_learn_armed ? "ARMED — send CC" : "K2 arm · send CC to map",
                       tack_learn_armed ? TACK_COL_LEARN : T_DIM, T_BG, 0);
        {
            char last[32];
            str_cpy(last, "last CC", sizeof last);
            {
                char n[8];
                fmt_int(n, tack_learn_last_cc);
                tack_str_cat(last, n, sizeof last);
                tack_str_cat(last, "=", sizeof last);
                fmt_int(n, tack_learn_last_val);
                tack_str_cat(last, n, sizeof last);
            }
            draw_text_line(10, 192, 220, &AF_S, last, T_MID, T_BG, 0);
        }
        draw_text_line(10, 212, 220, &AF_S,
                       (song.playing || tack_is_locked()) ? "CLR blocked while PLAY/LOCK" : "HOME · map sticks",
                       T_DIM, T_BG, 0);
        return;
    }

    if (pg == TACK_PG_ZEN) {
        /* 5.9 Zen — one CALM knob, Chaos off, sombre */
        {
            uint16_t zbg = ux_mix(T_BG, TACK_COL_ZEN, 22);
            cv_rect(8, 40, 224, 120, zbg);
            draw_text_line(10, 48, 220, &AF_L, "ZEN", TACK_COL_ZEN, zbg, 0);
            {
                char line[24];
                str_cpy(line, "CALM  ", sizeof line);
                {
                    char n[8];
                    fmt_int(n, tack_zen_calm);
                    tack_str_cat(line, n, sizeof line);
                }
                draw_text_line(10, 100, 220, &AF_L, line, T_TEXT, zbg, 0);
            }
            {
                int32_t bw = (int32_t)((uint32_t)tack_zen_calm * 208u / 127u);
                cv_rect(16, 140, 208, 8, ux_mix(T_BG, TACK_COL_ZEN, 30));
                if (bw > 0) cv_rect(16, 140, bw, 8, TACK_COL_ZEN);
            }
            draw_text_line(10, 160, 220, &AF_S, "morph · filter + reverb + level", T_MID, zbg, 0);
        }
        draw_text_line(10, 204, 220, &AF_S, "K1 MORPH · K2/K4 exit", T_DIM, T_BG, 0);
        return;
    }

    if (pg == TACK_PG_GEN) {
        /* Generative — spare list, accent when Chaos on. Soft header-only redraw. */
        static const char *const GEN[7] = {
            "AUTO", "CHAOS", "DENSITY", "SPARE", "AIR MORPH", "AIR FREEZE", "VARIATION"
        };
        draw_text_line(10, 40, 220, &AF_S, "K1 row  K2 toggle  K3 set  K4→ZEN", T_DIM, T_BG, 0);
        if (tack_chaos)
            draw_text_line(184, 40, 48, &AF_S, "RND", T_ACCENT, T_BG, 2);
        for (i = 0; i < 7u; i++) {
            y = 56u + i * 18u;
            uint8_t on = (i == tack_gen_sel);
            uint16_t row_bg = on ? ux_mix(T_BG, TACK_COL_LIVE, 58) : T_BG;
            uint16_t row_ink = on ? T_INK : T_TEXT;
            cv_rect(8, (int32_t)y - 1, 224, 17, row_bg); /* 6.24 soft */
            if (on) cv_rect(8, (int32_t)y - 1, 3, 17, TACK_COL_LIVE);
            draw_text_line(14, y, 110, &AF_S, GEN[i], row_ink, row_bg, 0);
            b[0] = 0;
            if (i == 0) str_cpy(b, tack_auto ? "ON" : "OFF", sizeof b);
            else if (i == 1) str_cpy(b, tack_chaos ? "ON" : "OFF", sizeof b);
            else if (i == 2) fmt_int(b, tack_dens);
            else if (i == 3) str_cpy(b, tack_drm_spare ? "ON" : "OFF", sizeof b);
            else if (i == 4) fmt_int(b, tack_air_morph);
            else if (i == 5) str_cpy(b, tack_air_freeze ? "ON" : "OFF", sizeof b);
            else if (i == 6) {
                if (!tack_move_rate) str_cpy(b, "OFF", sizeof b);
                else fmt_int(b, tack_move_rate);
            }
            {
                uint16_t vcol = row_ink;
                if (i == 1 && tack_chaos) vcol = on ? T_INK : T_ACCENT;
                if (i == 0 && tack_auto) vcol = on ? T_INK : T_THEME;
                draw_text_line(150, y, 70, &AF_S, b, vcol, row_bg, 2);
            }
        }
        draw_text_line(10, 200, 220, &AF_S, "Chaos off by default · VAR = every N bars", T_DIM, T_BG, 0);
        return;
    }
}
