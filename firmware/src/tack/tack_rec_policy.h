/*
 * TACK review candidate — recording event classification + scale adapt.
 *
 * Host-testable standalone module. Not wired into the live 6.27 firmware tree
 * or historical pack binaries.
 *
 * Authorized default (Nicolas, 2026-11-09): TACK_REC_ONSET_CLUSTER.
 * Only notes whose onsets fall within TACK_REC_DEFAULT_ONSET_WINDOW_MS of the
 * cluster's earliest onset, AND that actually overlap in time, form a chord.
 * Legato / sustain overlap alone is melody (follows selected scale).
 * TACK_REC_SUSTAIN_OVERLAP remains for contrast tests only — not shipped.
 *
 * Threshold: 80 ms from the first onset of a cluster (not pairwise chaining).
 * Rationale: typical piano roll 20–80 ms; firmware already debounces identical
 * captured chords at ~90 ms (tack_crec_capture). An 81 ms onset vs the first
 * press is not part of that chord. Pairwise 80 ms hops (0,70,140,…) must not
 * swallow a melody into one chord.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef TACK_REC_POLICY_H
#define TACK_REC_POLICY_H

#include <stdint.h>

#define TACK_REC_MAX_NOTES 32
#define TACK_REC_MAX_EVENTS 32
#define TACK_REC_MAX_VOICES 8
#define TACK_REC_DEFAULT_ONSET_WINDOW_MS 80u
#define TACK_REC_DEFAULT_MIN_CHORD_NOTES 2u

typedef enum {
    TACK_REC_ONSET_CLUSTER = 0,   /* authorized: close onsets + actual overlap */
    TACK_REC_SUSTAIN_OVERLAP = 1  /* pending/ambiguous: any sustain overlap = chord */
} TackRecPolicy;

typedef enum {
    TACK_EV_MONO = 0,
    TACK_EV_CHORD = 1
} TackEvKind;

typedef struct {
    uint8_t pitch;       /* MIDI note 0..127 */
    uint32_t onset_ms;
    uint32_t off_ms;     /* exclusive end; must be > onset_ms */
} TackNoteEv;

typedef struct {
    TackEvKind kind;
    uint32_t members; /* raw indices, only set by classifier */
    uint8_t n;
    uint8_t pitches[TACK_REC_MAX_VOICES]; /* performed pitches, sorted low..high */
    uint32_t onset_ms;                    /* earliest onset in the group */
    uint32_t off_ms;                      /* latest release in the group */
} TackClassifiedEv;

/* Raw performed pitches vs scale-mapped playback pitches (bookkeeping). */
typedef struct {
    TackEvKind kind;
    uint8_t n;
    uint8_t raw[TACK_REC_MAX_VOICES];
    uint8_t mapped[TACK_REC_MAX_VOICES];
} TackPlaybackMap;

/* Incremental recorder: note-on / note-off → raw TackNoteEv list. */
typedef struct {
    TackNoteEv raw[TACK_REC_MAX_NOTES];
    int nraw;
    uint8_t raw_channel[TACK_REC_MAX_NOTES];
    uint8_t active_channel[TACK_REC_MAX_NOTES], active_pitch[TACK_REC_MAX_NOTES];
    uint32_t active_start[TACK_REC_MAX_NOTES];
    int active_n;
    uint8_t dropped;
    uint8_t live_on[128];
    uint32_t live_onset[128];
} TackRecSession;

typedef struct {
    uint8_t tonic;           /* 0..11, C=0 */
    uint8_t interval_count;
    uint8_t intervals[12];   /* scale degrees as semitone offsets from tonic */
} TackScale;

/* C major / C natural minor — matches tack_data.c mode tables. */
extern const TackScale TACK_SCALE_C_MAJOR;
extern const TackScale TACK_SCALE_C_MINOR;

/* Classify raw note events into mono lines vs temporal chords.
 * Returns number of classified events, or -1 on bad input.
 * Mixed melody+chord clips are split per group (not whole-clip).
 * Serial multi-pitch with no overlap is never a chord. */
int tack_rec_classify(const TackNoteEv *notes, int nnotes,
                      TackRecPolicy policy, uint32_t onset_window_ms,
                      TackClassifiedEv *out, int out_cap);

/* Snap one MIDI pitch to the nearest in-scale pitch class.
 * Ties (equal distance) flatten: prefer the lower absolute MIDI note.
 * Already in-scale pitches are unchanged. Search spans octave boundaries
 * and returns an in-scale note within 0..127. */
uint8_t tack_rec_adapt_pitch(uint8_t pitch, const TackScale *scale);

/* Playback-time rendering: mono events adapt; chord events keep performed pitches. */
int tack_rec_playback_pitches(const TackClassifiedEv *ev, const TackScale *scale,
                              uint8_t *out, int out_cap);

/* Firmware-faithful quality guess (tack_kb_quality). Used only to demonstrate
 * current lossy {root, quality} capture — not for the proposed path.
 * n = 0 returns 23 (TACK_Q_CLUSTER). */
uint8_t tack_rec_fw_kb_quality(const uint8_t *n, uint8_t cnt);

/* Model of current firmware capture decision on a held-key bitmask change.
 * Returns: 0 = ignore (1–2 keys, or single key consumed as a move pick),
 *          1 = custom chord stored as {root=lowest%12, quality=kb_quality}.
 * If page_is_home_or_loop and exactly one key 0..5 is newly pressed while
 * only that key is held, firmware treats it as a MOVE pick (not a note). */
int tack_rec_fw_would_capture_chord(uint32_t held_notes, uint32_t rising,
                                    int page_is_home_or_loop);

/* Fill raw + mapped. Chord: mapped == raw (absolute voicing). Mono: mapped = adapt. */
int tack_rec_map_event(const TackClassifiedEv *ev, const TackScale *scale,
                       TackPlaybackMap *out);

void tack_rec_session_init(TackRecSession *s);
/* Note-on: starts a raw event. Retrigger of a held pitch closes the old
 * event just before t_ms when possible. Returns 0 ok, -1 overflow/bad. */
int tack_rec_session_note_on(TackRecSession *s, uint8_t pitch, uint32_t t_ms);
/* Note-off: exclusive end at t_ms. Unknown pitch is ignored (returns 0).
 * If t_ms == onset, end is onset+1 so the event stays valid and point-like. */
int tack_rec_session_note_off(TackRecSession *s, uint8_t pitch, uint32_t t_ms);
/* Close any still-held notes at t_ms (stop / REC end). */
int tack_rec_session_flush(TackRecSession *s, uint32_t t_ms);

/* If ms collides with any have[i], bump until unique. */
uint32_t tack_rec_uniquify_ms(const uint32_t *have, int nhave, uint32_t ms);

int tack_rec_session_channel(TackRecSession *, uint8_t ch, uint8_t pitch, uint8_t vel, uint32_t ms);
int tack_rec_session_channel_flush(TackRecSession *, uint32_t ms);
#endif
