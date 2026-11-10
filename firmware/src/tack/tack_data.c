#include "tack.h"

static const TackModeInfo kModes[] = {
    { TACK_MODE_MAJOR,     "MAJOR",     "open and resolved",       7, { 0,2,4,5,7,9,11 }, { TACK_Q_MAJ,TACK_Q_MIN,TACK_Q_MIN,TACK_Q_MAJ,TACK_Q_MAJ,TACK_Q_MIN,TACK_Q_DIM } },
    { TACK_MODE_MINOR,     "MINOR",     "the classic minor",       7, { 0,2,3,5,7,8,10 }, { TACK_Q_MIN,TACK_Q_DIM,TACK_Q_MAJ,TACK_Q_MIN,TACK_Q_MIN,TACK_Q_MAJ,TACK_Q_MAJ } },
    { TACK_MODE_DORIAN,    "DORIAN",    "minor with a lifted sixth",7, { 0,2,3,5,7,9,10 }, { TACK_Q_MIN,TACK_Q_MIN,TACK_Q_MAJ,TACK_Q_MAJ,TACK_Q_MIN,TACK_Q_DIM,TACK_Q_MAJ } },
    { TACK_MODE_PHRYGIAN,  "PHRYGIAN",  "flat second, dark",       7, { 0,1,3,5,7,8,10 }, { TACK_Q_MIN,TACK_Q_MAJ,TACK_Q_MAJ,TACK_Q_MIN,TACK_Q_DIM,TACK_Q_MAJ,TACK_Q_MIN } },
    { TACK_MODE_LYDIAN,    "LYDIAN",    "raised fourth, bright",   7, { 0,2,4,6,7,9,11 }, { TACK_Q_MAJ,TACK_Q_MAJ,TACK_Q_MIN,TACK_Q_DIM,TACK_Q_MAJ,TACK_Q_MIN,TACK_Q_MIN } },
    { TACK_MODE_CHINESE,   "CHINESE",   "major pentatonic, open",  5, { 0,2,4,7,9 },      { TACK_Q_MAJ,TACK_Q_MIN,TACK_Q_MIN,TACK_Q_MAJ,TACK_Q_MIN } },
    { TACK_MODE_CANTOPOP,  "CANTOPOP",  "bittersweet, melodic",    7, { 0,2,4,5,7,9,11 }, { TACK_Q_MAJ,TACK_Q_MIN,TACK_Q_MIN,TACK_Q_MAJ,TACK_Q_MAJ,TACK_Q_MIN,TACK_Q_DIM } },
    { TACK_MODE_JAPANESE,  "JAPANESE",  "hirajoshi, austere",      5, { 0,2,3,7,8 },      { TACK_Q_MIN,TACK_Q_MAJ,TACK_Q_MAJ,TACK_Q_MIN,TACK_Q_MAJ } },
    { TACK_MODE_WHOLETONE, "WHOLE TONE","six equal steps, floating",6,{ 0,2,4,6,8,10 },   { TACK_Q_AUG,TACK_Q_AUG,TACK_Q_AUG,TACK_Q_DOM7S5,TACK_Q_AUG,TACK_Q_AUG } },
    { TACK_MODE_OCTATONIC, "OCTATONIC", "eight-note symmetry",     8, { 0,1,3,4,6,7,9,10 },{ TACK_Q_DIM7,TACK_Q_MAJ,TACK_Q_MIN,TACK_Q_DIM7,TACK_Q_MAJ,TACK_Q_MIN,TACK_Q_DIM7,TACK_Q_MAJ } },
    { TACK_MODE_LOCRIAN,   "LOCRIAN",   "unstable, dark",          7, { 0,1,3,5,6,8,10 }, { TACK_Q_DIM,TACK_Q_MAJ,TACK_Q_MIN,TACK_Q_MIN,TACK_Q_MAJ,TACK_Q_MAJ,TACK_Q_MIN } },
    { TACK_MODE_ULTRA,     "ULTRA",     "chromatic clusters",     12, { 0,1,2,3,4,5,6,7,8,9,10,11 }, { TACK_Q_CLUSTER,TACK_Q_CLUSTER,TACK_Q_CLUSTER,TACK_Q_CLUSTER,TACK_Q_CLUSTER,TACK_Q_CLUSTER,TACK_Q_CLUSTER,TACK_Q_CLUSTER,TACK_Q_CLUSTER,TACK_Q_CLUSTER,TACK_Q_CLUSTER,TACK_Q_CLUSTER } },
    { TACK_MODE_DISSONANT, "DISSONANT", "hard edges, no home",     8, { 0,1,2,3,6,7,8,11 },{ TACK_Q_DIM,TACK_Q_MAJ,TACK_Q_MIN,TACK_Q_DIM,TACK_Q_MAJ,TACK_Q_MIN,TACK_Q_MAJ,TACK_Q_DIM } },
    { TACK_MODE_HARMONIC_MINOR, "HARMONIC MINOR", "classical tension", 7, { 0,2,3,5,7,8,11 }, { TACK_Q_MIN,TACK_Q_DIM,TACK_Q_MAJ,TACK_Q_MIN,TACK_Q_MAJ,TACK_Q_MAJ,TACK_Q_DIM } },
    { TACK_MODE_PHRYGIAN_DOMINANT, "PHRYGIAN DOM", "Spanish tension", 7, { 0,1,4,5,7,8,10 }, { TACK_Q_MAJ,TACK_Q_MAJ,TACK_Q_DIM,TACK_Q_MIN,TACK_Q_MAJ,TACK_Q_MAJ,TACK_Q_MIN } },
    { TACK_MODE_BLUES, "BLUES", "minor + blue note, I-IV-V", 7, { 0,2,3,5,6,7,10 }, { TACK_Q_MIN,TACK_Q_DIM,TACK_Q_MAJ,TACK_Q_DOM7,TACK_Q_DIM,TACK_Q_DOM7,TACK_Q_MAJ } },
    { TACK_MODE_HEXATONIC_BLUES, "HEX BLUES", "classic 6-note blues", 6, { 0,3,5,6,7,10 }, { TACK_Q_MIN,TACK_Q_MAJ,TACK_Q_DOM7,TACK_Q_DIM,TACK_Q_DOM7,TACK_Q_MAJ } },
    { TACK_MODE_GLASS, "GLASS", "additive arpeggio ostinato", 5, { 0,2,4,7,11 }, { TACK_Q_MAJ7,TACK_Q_ADD9,TACK_Q_MIN,TACK_Q_MAJ,TACK_Q_MAJ7 } },
    { TACK_MODE_REICH, "REICH", "phase pulse interlocking", 5, { 0,2,5,7,9 }, { TACK_Q_SUS2,TACK_Q_MAJ,TACK_Q_SUS4,TACK_Q_MAJ,TACK_Q_MIN } },
    { TACK_MODE_RILEY, "RILEY", "In C pulsed modal cells", 6, { 0,2,4,5,7,9 }, { TACK_Q_MAJ,TACK_Q_SUS2,TACK_Q_MIN,TACK_Q_ADD9,TACK_Q_MAJ,TACK_Q_SUS4 } },
    { TACK_MODE_ADAMS, "ADAMS", "post-minimal pulse drive", 7, { 0,2,3,5,7,9,10 }, { TACK_Q_MIN,TACK_Q_MIN7,TACK_Q_MAJ,TACK_Q_MIN,TACK_Q_MAJ,TACK_Q_MIN7,TACK_Q_DOM7 } }
};

static const TackQualityInfo kQualities[] = {
    { TACK_Q_MIN,     "m",       "min",    3, { 0,3,7 },       TACK_FAMILY_MINOR },
    { TACK_Q_MAJ,     "",        "maj",    3, { 0,4,7 },       TACK_FAMILY_MAJOR },
    { TACK_Q_DIM,     "°",       "dim",    3, { 0,3,6 },       TACK_FAMILY_DIM },
    { TACK_Q_MIN7,    "m7",      "min7",   4, { 0,3,7,10 },    TACK_FAMILY_MINOR },
    { TACK_Q_MIN9,    "m9",      "min9",   5, { 0,3,7,10,14 }, TACK_FAMILY_MINOR },
    { TACK_Q_MAJ7,    "maj7",    "maj7",   4, { 0,4,7,11 },    TACK_FAMILY_MAJOR },
    { TACK_Q_MAJ9,    "maj9",    "maj9",   5, { 0,4,7,11,14 }, TACK_FAMILY_MAJOR },
    { TACK_Q_DOM7,    "7",       "7",      4, { 0,4,7,10 },    TACK_FAMILY_DOM },
    { TACK_Q_DOM9,    "9",       "9",      5, { 0,4,7,10,14 }, TACK_FAMILY_DOM },
    { TACK_Q_SUS2,    "sus2",    "sus2",   3, { 0,2,7 },       TACK_FAMILY_SUS },
    { TACK_Q_SUS4,    "sus4",    "sus4",   3, { 0,5,7 },       TACK_FAMILY_SUS },
    { TACK_Q_MIN6,    "m6",      "m6",     4, { 0,3,7,9 },     TACK_FAMILY_MINOR },
    { TACK_Q_M7B5,    "m7b5",    "m7b5",   4, { 0,3,6,10 },    TACK_FAMILY_DIM },
    { TACK_Q_ADD9,    "add9",    "add9",   4, { 0,4,7,14 },    TACK_FAMILY_MAJOR },
    { TACK_Q_MIN11,   "m11",     "m11",    6, { 0,3,7,10,14,17 }, TACK_FAMILY_MINOR },
    { TACK_Q_MAJ7S11, "maj7s11", "maj7s11",5, { 0,4,7,11,18 }, TACK_FAMILY_MAJOR },
    { TACK_Q_DOM13,   "13",      "13",     5, { 0,4,7,10,21 }, TACK_FAMILY_DOM },
    { TACK_Q_SUS13,   "sus13",   "sus13",  5, { 0,5,7,10,21 }, TACK_FAMILY_SUS },
    { TACK_Q_SIX9,    "6/9",     "6/9",    5, { 0,4,7,9,14 },  TACK_FAMILY_MAJOR },
    { TACK_Q_AUG,     "+",       "aug",    3, { 0,4,8 },       TACK_FAMILY_AUG },
    { TACK_Q_DIM7,    "°7",      "dim7",   4, { 0,3,6,9 },     TACK_FAMILY_DIM },
    { TACK_Q_DOM7S5,  "7#5",     "7#5",    4, { 0,4,8,10 },    TACK_FAMILY_DOM },
    { TACK_Q_DOM7B9,  "7b9",     "7b9",    5, { 0,4,7,10,13 }, TACK_FAMILY_DOM },
    { TACK_Q_CLUSTER, " cluster", "cluster",4,{ 0,1,6,11 },    TACK_FAMILY_CLUSTER },
    { TACK_Q_QUARTAL, " quartal", "quartal",4,{ 0,5,10,15 },   TACK_FAMILY_QUARTAL }
};

static const char *kStyleLabels[TACK_STYLE_COUNT] = {
    "CLASSIC", "JAZZ", "JUNGLE", "REGGAE", "AMBIENT", "DUB", "BREAKS", "BOSSA", "WALTZ", "FUNK", "BEATLES"
};

uint8_t tack_mode_count(void) { return (uint8_t)(sizeof(kModes) / sizeof(kModes[0])); }

const TackModeInfo *tack_mode_info(uint8_t mode) {
    if (mode >= tack_mode_count()) mode = 0;
    return &kModes[mode];
}

const TackQualityInfo *tack_quality_info(uint8_t quality) {
    if (quality >= (uint8_t)(sizeof(kQualities) / sizeof(kQualities[0]))) quality = 0;
    return &kQualities[quality];
}

const char *tack_style_label(uint8_t style) {
    if (style >= TACK_STYLE_COUNT) style = 0;
    return kStyleLabels[style];
}

const char *tack_mode_label(uint8_t mode) { return tack_mode_info(mode)->label; }
const char *tack_chord_suffix(uint8_t quality) { return tack_quality_info(quality)->suffix; }
