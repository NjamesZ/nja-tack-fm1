/*
 * TACK for FM-1 — portable harmony core
 *
 * No allocation, no floating point, no host dependencies. This is the part
 * that has to be identical on the FM-1 and in the browser: modes, chord
 * qualities, four-slot takes, bridges and six next moves.
 */
#ifndef TACK_H
#define TACK_H

#include <stdint.h>

#define TACK_SLOTS 4
#define TACK_MOVES 6
#define TACK_MAX_INTERVALS 12

typedef enum {
    TACK_STYLE_CLASSIC = 0,
    TACK_STYLE_JAZZ,
    TACK_STYLE_JUNGLE,
    TACK_STYLE_REGGAE,
    TACK_STYLE_AMBIENT,
    TACK_STYLE_DUB,
    TACK_STYLE_BREAKS,
    TACK_STYLE_BOSSA,
    TACK_STYLE_WALTZ,
    TACK_STYLE_FUNK,
    TACK_STYLE_BEATLES,
    TACK_STYLE_COUNT
} TackStyle;

typedef enum {
    TACK_MODE_MAJOR = 0,
    TACK_MODE_MINOR,
    TACK_MODE_DORIAN,
    TACK_MODE_PHRYGIAN,
    TACK_MODE_LYDIAN,
    TACK_MODE_CHINESE,
    TACK_MODE_CANTOPOP,
    TACK_MODE_JAPANESE,
    TACK_MODE_WHOLETONE,
    TACK_MODE_OCTATONIC,
    TACK_MODE_LOCRIAN,
    TACK_MODE_ULTRA,
    TACK_MODE_DISSONANT,
    TACK_MODE_HARMONIC_MINOR,
    TACK_MODE_PHRYGIAN_DOMINANT,
    TACK_MODE_BLUES,
    TACK_MODE_HEXATONIC_BLUES,
    TACK_MODE_GLASS,      /* Philip Glass: additive arpeggio cells, slow shift */
    TACK_MODE_REICH,      /* Steve Reich: phase/pulse interlocking, clear grid */
    TACK_MODE_RILEY,      /* Terry Riley: In C pulsed modal cells */
    TACK_MODE_ADAMS,      /* John Adams: post-minimal pulsing drive */
    TACK_MODE_COUNT
} TackMode;

typedef enum {
    TACK_Q_MIN = 0,
    TACK_Q_MAJ,
    TACK_Q_DIM,
    TACK_Q_MIN7,
    TACK_Q_MIN9,
    TACK_Q_MAJ7,
    TACK_Q_MAJ9,
    TACK_Q_DOM7,
    TACK_Q_DOM9,
    TACK_Q_SUS2,
    TACK_Q_SUS4,
    TACK_Q_MIN6,
    TACK_Q_M7B5,
    TACK_Q_ADD9,
    TACK_Q_MIN11,
    TACK_Q_MAJ7S11,
    TACK_Q_DOM13,
    TACK_Q_SUS13,
    TACK_Q_SIX9,
    TACK_Q_AUG,
    TACK_Q_DIM7,
    TACK_Q_DOM7S5,
    TACK_Q_DOM7B9,
    TACK_Q_CLUSTER,
    TACK_Q_QUARTAL,
    TACK_Q_COUNT
} TackQuality;

typedef enum {
    TACK_FAMILY_MINOR = 0,
    TACK_FAMILY_MAJOR,
    TACK_FAMILY_DIM,
    TACK_FAMILY_DOM,
    TACK_FAMILY_SUS,
    TACK_FAMILY_AUG,
    TACK_FAMILY_CLUSTER,
    TACK_FAMILY_QUARTAL
} TackFamily;

typedef struct {
    uint8_t root;       /* 0..11 */
    uint8_t quality;    /* TackQuality */
} TackChord;

typedef struct {
    uint8_t id;
    const char *label;
    const char *desc;
    uint8_t count;
    uint8_t intervals[TACK_MAX_INTERVALS];
    uint8_t qualities[TACK_MAX_INTERVALS];
} TackModeInfo;

typedef struct {
    uint8_t id;
    const char *suffix;
    const char *label;
    uint8_t count;
    uint8_t intervals[6];
    uint8_t family;
} TackQualityInfo;

typedef struct {
    uint8_t root;
    uint8_t mode;
    uint8_t style;

    uint32_t seed;
    TackChord chords[TACK_SLOTS];
    uint8_t current;
    uint8_t focus;

    uint8_t bridge_on;
    TackChord bridge_saved[TACK_SLOTS];
    uint8_t bridge_chords[TACK_SLOTS];
} TackState;

/* Mode / quality tables. */
uint8_t tack_mode_count(void);
const TackModeInfo *tack_mode_info(uint8_t mode);
const TackQualityInfo *tack_quality_info(uint8_t quality);
const char *tack_style_label(uint8_t style);

/* Lifecycle. */
void tack_init(TackState *s, uint8_t root, uint8_t mode, uint8_t style, uint32_t seed);
void tack_build_take(TackState *s);
void tack_set_root(TackState *s, uint8_t root);
void tack_set_mode(TackState *s, uint8_t mode);
void tack_set_style(TackState *s, uint8_t style); /* mode is preserved */
/* Keep[i] != 0: leave chords[i] untouched (user-captured / applied).
 * keep == NULL keeps none — same as the unlocked setters. Generated
 * (unowned) slots are rebuilt from style banks. */
void tack_build_take_keep(TackState *s, const uint8_t keep[TACK_SLOTS]);
void tack_set_root_keep(TackState *s, uint8_t root, const uint8_t keep[TACK_SLOTS]);
void tack_set_mode_keep(TackState *s, uint8_t mode, const uint8_t keep[TACK_SLOTS]);
void tack_set_style_keep(TackState *s, uint8_t style, const uint8_t keep[TACK_SLOTS]);

/* Bridge: in / out, restoring the normal take exactly. */
int tack_bridge_toggle(TackState *s);
void tack_bridge_out(TackState *s);

/* Six root-diverse next moves for the focused chord. Returns 0 on success. */
int tack_next_moves(const TackState *s, TackChord out[TACK_MOVES]);

/* Rootless comp voicing, four MIDI notes sorted low to high. Returns count. */
int tack_chord_notes(const TackChord *c, uint8_t out[4]);

/* Cantopop hook: one note per motif step, -1 = rest. */
int tack_cantopop_hook_note(const TackState *s, uint8_t bar, uint8_t step);

/* Utility. */
uint8_t tack_mod12(int value);
const char *tack_chord_suffix(uint8_t quality);
const char *tack_mode_label(uint8_t mode);

#endif
