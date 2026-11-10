#include "tack.h"

typedef struct {
    uint8_t degree;
    uint8_t quality;
} DegreeSpec;

static const DegreeSpec kMajorBanks[3][TACK_SLOTS] = {
    { {0,TACK_Q_MAJ9}, {4,TACK_Q_DOM7}, {5,TACK_Q_MIN9}, {3,TACK_Q_MAJ9} },
    { {5,TACK_Q_MIN9}, {3,TACK_Q_MAJ7}, {0,TACK_Q_ADD9}, {4,TACK_Q_DOM7} },
    { {0,TACK_Q_MAJ9}, {5,TACK_Q_MIN7}, {3,TACK_Q_MAJ9}, {4,TACK_Q_SUS13} }
};
static const DegreeSpec kMinorBanks[3][TACK_SLOTS] = {
    { {0,TACK_Q_MIN9}, {5,TACK_Q_MAJ9}, {2,TACK_Q_MAJ9}, {6,TACK_Q_SUS13} },
    { {0,TACK_Q_MIN9}, {3,TACK_Q_MIN11}, {5,TACK_Q_MAJ9}, {6,TACK_Q_SUS13} },
    { {0,TACK_Q_MIN9}, {5,TACK_Q_MAJ9}, {3,TACK_Q_MIN11}, {2,TACK_Q_MAJ9} }
};
static const DegreeSpec kDorianBanks[3][TACK_SLOTS] = {
    { {0,TACK_Q_MIN9}, {3,TACK_Q_DOM13}, {0,TACK_Q_MIN9}, {3,TACK_Q_DOM13} },
    { {0,TACK_Q_MIN9}, {3,TACK_Q_DOM7}, {0,TACK_Q_MIN9}, {5,TACK_Q_MAJ9} },
    { {0,TACK_Q_MIN9}, {5,TACK_Q_MAJ9}, {0,TACK_Q_MIN9}, {3,TACK_Q_DOM7} }
};
static const DegreeSpec kPhrygianBanks[3][TACK_SLOTS] = {
    { {0,TACK_Q_MIN}, {1,TACK_Q_MAJ}, {3,TACK_Q_MIN}, {6,TACK_Q_MAJ} },
    { {0,TACK_Q_MIN}, {6,TACK_Q_MIN}, {3,TACK_Q_MIN}, {1,TACK_Q_MAJ} },
    { {0,TACK_Q_MIN}, {1,TACK_Q_MAJ}, {6,TACK_Q_MIN}, {5,TACK_Q_MAJ} }
};
static const DegreeSpec kLydianBanks[3][TACK_SLOTS] = {
    { {0,TACK_Q_MAJ}, {1,TACK_Q_MAJ}, {4,TACK_Q_MIN}, {5,TACK_Q_MAJ} },
    { {0,TACK_Q_MAJ}, {4,TACK_Q_MIN}, {5,TACK_Q_MIN}, {1,TACK_Q_MAJ} },
    { {0,TACK_Q_MAJ}, {3,TACK_Q_DIM}, {4,TACK_Q_MIN}, {5,TACK_Q_MAJ} }
};
static const DegreeSpec kChineseBanks[3][TACK_SLOTS] = {
    { {0,TACK_Q_MAJ}, {3,TACK_Q_MAJ}, {4,TACK_Q_MIN}, {2,TACK_Q_MIN} },
    { {0,TACK_Q_MAJ}, {2,TACK_Q_MIN}, {4,TACK_Q_MIN}, {3,TACK_Q_MAJ} },
    { {0,TACK_Q_MAJ}, {3,TACK_Q_MAJ}, {1,TACK_Q_MIN}, {4,TACK_Q_MIN} }
};
static const DegreeSpec kCantopopBanks[3][TACK_SLOTS] = {
    { {0,TACK_Q_MAJ9}, {4,TACK_Q_DOM7}, {5,TACK_Q_MIN9}, {2,TACK_Q_MIN7} },
    { {5,TACK_Q_MIN9}, {3,TACK_Q_MAJ7}, {0,TACK_Q_ADD9}, {4,TACK_Q_DOM7} },
    { {0,TACK_Q_MAJ9}, {5,TACK_Q_MIN7}, {3,TACK_Q_MAJ7}, {4,TACK_Q_SUS13} }
};
static const DegreeSpec kJapaneseBanks[3][TACK_SLOTS] = {
    { {0,TACK_Q_MIN}, {2,TACK_Q_MAJ}, {3,TACK_Q_MIN}, {4,TACK_Q_MAJ} },
    { {0,TACK_Q_MIN}, {4,TACK_Q_MAJ}, {3,TACK_Q_MIN}, {1,TACK_Q_MAJ} },
    { {0,TACK_Q_MIN}, {1,TACK_Q_MAJ}, {2,TACK_Q_MAJ}, {4,TACK_Q_MAJ} }
};
static const DegreeSpec kWholeToneBanks[3][TACK_SLOTS] = {
    { {0,TACK_Q_AUG}, {2,TACK_Q_AUG}, {4,TACK_Q_AUG}, {3,TACK_Q_DOM7S5} },
    { {0,TACK_Q_AUG}, {3,TACK_Q_DOM7S5}, {5,TACK_Q_AUG}, {2,TACK_Q_AUG} },
    { {0,TACK_Q_DOM7S5}, {2,TACK_Q_AUG}, {4,TACK_Q_AUG}, {1,TACK_Q_AUG} }
};
static const DegreeSpec kOctatonicBanks[3][TACK_SLOTS] = {
    { {0,TACK_Q_DIM7}, {3,TACK_Q_DIM7}, {6,TACK_Q_DIM7}, {7,TACK_Q_MAJ} },
    { {0,TACK_Q_DIM7}, {6,TACK_Q_DIM7}, {3,TACK_Q_MAJ}, {1,TACK_Q_MAJ} },
    { {0,TACK_Q_DIM7}, {2,TACK_Q_MIN}, {5,TACK_Q_MAJ}, {7,TACK_Q_DIM7} }
};
static const DegreeSpec kLocrianBanks[3][TACK_SLOTS] = {
    { {0,TACK_Q_DIM}, {3,TACK_Q_MIN}, {6,TACK_Q_MAJ}, {1,TACK_Q_MAJ} },
    { {0,TACK_Q_DIM}, {4,TACK_Q_MAJ}, {1,TACK_Q_MAJ}, {6,TACK_Q_MAJ} },
    { {0,TACK_Q_DIM}, {6,TACK_Q_MAJ}, {3,TACK_Q_MIN}, {4,TACK_Q_MAJ} }
};
static const DegreeSpec kUltraBanks[3][TACK_SLOTS] = {
    { {0,TACK_Q_CLUSTER}, {3,TACK_Q_CLUSTER}, {6,TACK_Q_CLUSTER}, {9,TACK_Q_CLUSTER} },
    { {0,TACK_Q_CLUSTER}, {5,TACK_Q_CLUSTER}, {10,TACK_Q_CLUSTER}, {3,TACK_Q_CLUSTER} },
    { {0,TACK_Q_CLUSTER}, {7,TACK_Q_CLUSTER}, {2,TACK_Q_CLUSTER}, {9,TACK_Q_CLUSTER} }
};
static const DegreeSpec kDissonantBanks[3][TACK_SLOTS] = {
    { {0,TACK_Q_DIM}, {3,TACK_Q_DIM}, {6,TACK_Q_MAJ}, {7,TACK_Q_DIM} },
    { {0,TACK_Q_DIM}, {6,TACK_Q_MAJ}, {3,TACK_Q_DIM}, {1,TACK_Q_MAJ} },
    { {0,TACK_Q_DIM}, {7,TACK_Q_DIM}, {2,TACK_Q_MIN}, {5,TACK_Q_MIN} }
};

static const DegreeSpec kHarmonicBanks[3][TACK_SLOTS] = {
    { {0,TACK_Q_MIN},{3,TACK_Q_MIN},{5,TACK_Q_MAJ},{4,TACK_Q_MAJ} },
    { {0,TACK_Q_MIN},{5,TACK_Q_MAJ},{4,TACK_Q_MAJ},{3,TACK_Q_MIN} },
    { {0,TACK_Q_MIN},{4,TACK_Q_MAJ},{5,TACK_Q_MAJ},{6,TACK_Q_DIM} }
};
static const DegreeSpec kPhrygianDomBanks[3][TACK_SLOTS] = {
    { {0,TACK_Q_MAJ},{1,TACK_Q_MAJ},{3,TACK_Q_MIN},{4,TACK_Q_DIM} },
    { {0,TACK_Q_MAJ},{4,TACK_Q_DIM},{1,TACK_Q_MAJ},{5,TACK_Q_MAJ} },
    { {0,TACK_Q_MAJ},{3,TACK_Q_MIN},{5,TACK_Q_MAJ},{1,TACK_Q_MAJ} }
};
/* Blues: spare I–IV–V (degrees 0,3=fourth,5=fifth on heptatonic blues). */
static const DegreeSpec kBluesBanks[3][TACK_SLOTS] = {
    { {0,TACK_Q_MIN7},{3,TACK_Q_DOM7},{0,TACK_Q_MIN7},{5,TACK_Q_DOM7} },
    { {0,TACK_Q_DOM7},{3,TACK_Q_DOM7},{5,TACK_Q_DOM7},{0,TACK_Q_MIN7} },
    { {0,TACK_Q_MIN9},{3,TACK_Q_DOM9},{0,TACK_Q_MIN7},{5,TACK_Q_DOM7} }
};
/* Hexatonic blues: degrees 0,2=IV(5),4=V(7). */
static const DegreeSpec kHexBluesBanks[3][TACK_SLOTS] = {
    { {0,TACK_Q_MIN7},{2,TACK_Q_DOM7},{0,TACK_Q_MIN7},{4,TACK_Q_DOM7} },
    { {0,TACK_Q_DOM7},{2,TACK_Q_DOM7},{4,TACK_Q_DOM7},{0,TACK_Q_MIN7} },
    { {0,TACK_Q_MIN},{2,TACK_Q_DOM7},{4,TACK_Q_DOM7},{0,TACK_Q_MIN7} }
};


/* Modern composers — spare sombre banks; slow harmonic shift. */
/* Glass: additive cells — I maj7 hangs, rare lift to V/ii. */
static const DegreeSpec kGlassBanks[3][TACK_SLOTS] = {
    { {0,TACK_Q_MAJ7},{0,TACK_Q_MAJ7},{4,TACK_Q_ADD9},{0,TACK_Q_MAJ7} },
    { {0,TACK_Q_MAJ7},{2,TACK_Q_ADD9},{0,TACK_Q_MAJ7},{4,TACK_Q_MAJ} },
    { {0,TACK_Q_ADD9},{0,TACK_Q_MAJ7},{2,TACK_Q_MIN},{0,TACK_Q_MAJ7} }
};
/* Reich: interlocking pulse — I↔V ostinato, sus clarity. */
static const DegreeSpec kReichBanks[3][TACK_SLOTS] = {
    { {0,TACK_Q_SUS2},{4,TACK_Q_MAJ},{0,TACK_Q_SUS2},{4,TACK_Q_MAJ} },
    { {0,TACK_Q_SUS4},{0,TACK_Q_SUS2},{4,TACK_Q_MAJ},{0,TACK_Q_SUS4} },
    { {0,TACK_Q_MAJ},{4,TACK_Q_SUS2},{0,TACK_Q_MAJ},{4,TACK_Q_SUS2} }
};
/* Riley: In C cells — modal I–ii–V loops, gradual colour. */
static const DegreeSpec kRileyBanks[3][TACK_SLOTS] = {
    { {0,TACK_Q_MAJ},{1,TACK_Q_SUS2},{0,TACK_Q_MAJ},{4,TACK_Q_ADD9} },
    { {0,TACK_Q_SUS2},{1,TACK_Q_MIN},{0,TACK_Q_MAJ},{4,TACK_Q_SUS4} },
    { {0,TACK_Q_MAJ},{0,TACK_Q_ADD9},{1,TACK_Q_SUS2},{4,TACK_Q_MAJ} }
};
/* Adams: post-minimal — more motion I–vi–IV–V, still spare. */
static const DegreeSpec kAdamsBanks[3][TACK_SLOTS] = {
    { {0,TACK_Q_MIN},{5,TACK_Q_MIN7},{3,TACK_Q_MAJ},{4,TACK_Q_MAJ} },
    { {0,TACK_Q_MIN},{3,TACK_Q_MAJ},{4,TACK_Q_DOM7},{0,TACK_Q_MIN} },
    { {0,TACK_Q_MIN7},{5,TACK_Q_MIN},{4,TACK_Q_MAJ},{3,TACK_Q_MAJ} }
};

static const DegreeSpec *const kBanks[TACK_MODE_COUNT] = {
    &kMajorBanks[0][0], &kMinorBanks[0][0], &kDorianBanks[0][0],
    &kPhrygianBanks[0][0], &kLydianBanks[0][0], &kChineseBanks[0][0],
    &kCantopopBanks[0][0], &kJapaneseBanks[0][0], &kWholeToneBanks[0][0],
    &kOctatonicBanks[0][0], &kLocrianBanks[0][0], &kUltraBanks[0][0],
    &kDissonantBanks[0][0],
    &kHarmonicBanks[0][0], &kPhrygianDomBanks[0][0], &kBluesBanks[0][0],
    &kHexBluesBanks[0][0], &kGlassBanks[0][0], &kReichBanks[0][0], &kRileyBanks[0][0], &kAdamsBanks[0][0]
};

uint8_t tack_mod12(int value) {
    int v = value % 12;
    return (uint8_t)(v < 0 ? v + 12 : v);
}

static uint32_t next_random(uint32_t *seed) {
    *seed = (*seed * 1664525u) + 1013904223u;
    return *seed;
}

static TackChord chord_at_degree(const TackState *s, uint8_t degree, uint8_t quality) {
    const TackModeInfo *m = tack_mode_info(s->mode);
    uint8_t d = degree % m->count;
    TackChord c;
    c.root = tack_mod12(s->root + m->intervals[d]);
    c.quality = quality < TACK_Q_COUNT ? quality : TACK_Q_MAJ;
    return c;
}

static void build_from_bank(TackState *s, const DegreeSpec *bank) {
    for (int i = 0; i < TACK_SLOTS; i++) {
        s->chords[i] = chord_at_degree(s, bank[i].degree, bank[i].quality);
    }
}

static void build_bridge(TackState *s) {
    const TackModeInfo *m = tack_mode_info(s->mode);
    if (s->mode == TACK_MODE_CANTOPOP) {
        static const uint8_t d[4] = { 5, 3, 4, 2 };
        static const uint8_t q[4] = { TACK_Q_MIN9, TACK_Q_MAJ7, TACK_Q_DOM7, TACK_Q_MIN7 };
        for (int i = 0; i < 4; i++) s->bridge_chords[i] = 0, s->chords[i] = chord_at_degree(s, d[i], q[i]);
        return;
    }
    uint8_t base = m->count >= 7 ? 5 : (uint8_t)(m->count - 1);
    uint8_t degree[4];
    degree[0] = base % m->count;
    degree[1] = (uint8_t)((base + m->count - 2) % m->count);
    degree[2] = 0;
    degree[3] = (uint8_t)((base + 2) % m->count);
    for (int i = 0; i < 4; i++) {
        s->bridge_chords[i] = 0;
        s->chords[i] = chord_at_degree(s, degree[i], m->qualities[degree[i]]);
    }
}

void tack_init(TackState *s, uint8_t root, uint8_t mode, uint8_t style, uint32_t seed) {
    if (!s) return;
    s->root = tack_mod12(root);
    s->mode = mode < TACK_MODE_COUNT ? mode : TACK_MODE_MAJOR;
    s->style = style < TACK_STYLE_COUNT ? style : TACK_STYLE_CLASSIC;
    s->seed = seed ? seed : 1;
    s->current = 0;
    s->focus = 0;
    s->bridge_on = 0;
    for (int i = 0; i < 4; i++) { s->bridge_saved[i].root = 0; s->bridge_saved[i].quality = 0; s->bridge_chords[i] = 0; }
    tack_build_take(s);
}

void tack_build_take(TackState *s) {
    const TackModeInfo *m = tack_mode_info(s->mode);
    uint8_t bank = (uint8_t)(next_random(&s->seed) % 3u);
    build_from_bank(s, kBanks[m->id] + (bank * TACK_SLOTS));
    s->current = 0;
    s->focus = 0;
    s->bridge_on = 0;
}

void tack_set_root(TackState *s, uint8_t root) {
    s->root = tack_mod12(root);
    tack_build_take(s);
}

void tack_set_mode(TackState *s, uint8_t mode) {
    if (mode >= TACK_MODE_COUNT) return;
    s->mode = mode;
    tack_build_take(s);
}

void tack_set_style(TackState *s, uint8_t style) {
    if (style >= TACK_STYLE_COUNT) return;
    s->style = style;          /* mode deliberately untouched */
    tack_build_take(s);
}

static void tack_restore_kept(TackState *s, const TackChord saved[TACK_SLOTS],
                              const uint8_t keep[TACK_SLOTS])
{
    int i;
    if (!s || !keep) return;
    for (i = 0; i < TACK_SLOTS; i++) {
        if (keep[i]) s->chords[i] = saved[i];
    }
}

void tack_build_take_keep(TackState *s, const uint8_t keep[TACK_SLOTS]) {
    TackChord saved[TACK_SLOTS];
    int i;
    if (!s) return;
    for (i = 0; i < TACK_SLOTS; i++) saved[i] = s->chords[i];
    tack_build_take(s);
    tack_restore_kept(s, saved, keep);
}

void tack_set_root_keep(TackState *s, uint8_t root, const uint8_t keep[TACK_SLOTS]) {
    TackChord saved[TACK_SLOTS];
    int i;
    if (!s) return;
    for (i = 0; i < TACK_SLOTS; i++) saved[i] = s->chords[i];
    s->root = tack_mod12(root);
    tack_build_take(s);
    tack_restore_kept(s, saved, keep);
}

void tack_set_mode_keep(TackState *s, uint8_t mode, const uint8_t keep[TACK_SLOTS]) {
    TackChord saved[TACK_SLOTS];
    int i;
    if (!s || mode >= TACK_MODE_COUNT) return;
    for (i = 0; i < TACK_SLOTS; i++) saved[i] = s->chords[i];
    s->mode = mode;
    tack_build_take(s);
    tack_restore_kept(s, saved, keep);
}

void tack_set_style_keep(TackState *s, uint8_t style, const uint8_t keep[TACK_SLOTS]) {
    TackChord saved[TACK_SLOTS];
    int i;
    if (!s || style >= TACK_STYLE_COUNT) return;
    for (i = 0; i < TACK_SLOTS; i++) saved[i] = s->chords[i];
    s->style = style;
    tack_build_take(s);
    tack_restore_kept(s, saved, keep);
}

int tack_bridge_toggle(TackState *s) {
    if (!s) return 0;
    if (s->bridge_on) {
        for (int i = 0; i < 4; i++) s->chords[i] = s->bridge_saved[i];
        s->bridge_on = 0;
        return 0;
    }
    for (int i = 0; i < 4; i++) s->bridge_saved[i] = s->chords[i];
    build_bridge(s);
    s->bridge_on = 1;
    return 1;
}

void tack_bridge_out(TackState *s) {
    if (!s || !s->bridge_on) return;
    for (int i = 0; i < 4; i++) s->chords[i] = s->bridge_saved[i];
    s->bridge_on = 0;
}

static int chord_equal(const TackChord *a, const TackChord *b) {
    return a->root == b->root && a->quality == b->quality;
}

int tack_next_moves(const TackState *s, TackChord out[TACK_MOVES]) {
    if (!s || !out) return -1;
    const TackModeInfo *m = tack_mode_info(s->mode);
    TackChord cand[24];
    int n = 0;
    TackChord current = s->chords[s->focus < TACK_SLOTS ? s->focus : 0];

    for (uint8_t i = 0; i < m->count && n < 20; i++) {
        TackChord c = chord_at_degree(s, i, m->qualities[i]);
        if (chord_equal(&c, &current)) continue;
        int duplicate = 0;
        for (int k = 0; k < n; k++) if (chord_equal(&cand[k], &c)) duplicate = 1;
        if (!duplicate) cand[n++] = c;
    }
    /* Borrowed colours mirror TACK's prediction set. */
    static const struct { uint8_t off, q; } borrowed[] = {
        {1,TACK_Q_MAJ7},{8,TACK_Q_MAJ7},{10,TACK_Q_MAJ},{10,TACK_Q_DOM7},{5,TACK_Q_SUS4},{7,TACK_Q_MAJ}
    };
    for (unsigned i = 0; i < sizeof(borrowed)/sizeof(borrowed[0]) && n < 24; i++) {
        TackChord c;
        c.root = tack_mod12(s->root + borrowed[i].off);
        c.quality = borrowed[i].q;
        if (chord_equal(&c, &current)) continue;
        int duplicate = 0;
        for (int k = 0; k < n; k++) if (chord_equal(&cand[k], &c)) duplicate = 1;
        if (!duplicate) cand[n++] = c;
    }

    /* Root-diverse selection: best score first, then quality diversity. */
    int used[12] = {0};
    int count = 0;
    for (int pass = 0; pass < 2 && count < TACK_MOVES; pass++) {
        for (int i = 0; i < n && count < TACK_MOVES; i++) {
            int already = 0;
            for (int k = 0; k < count; k++) {
                if (out[k].root == cand[i].root && out[k].quality == cand[i].quality) already = 1;
            }
            if (already) continue;
            if (pass == 0 && used[cand[i].root]) continue;
            out[count++] = cand[i];
            used[cand[i].root] = 1;
        }
    }
    /* Fill in case a tiny mode can't produce six unique roots. */
    for (int i = 0; i < n && count < TACK_MOVES; i++) {
        int already = 0;
        for (int k = 0; k < count; k++) {
            if (out[k].root == cand[i].root && out[k].quality == cand[i].quality) already = 1;
        }
        if (!already) out[count++] = cand[i];
    }
    while (count < TACK_MOVES) {
        out[count].root = tack_mod12(current.root + 5 + count);
        out[count].quality = TACK_Q_SUS2;
        count++;
    }
    return 0;
}

int tack_chord_notes(const TackChord *c, uint8_t out[4]) {
    if (!c || !out) return 0;
    const TackQualityInfo *q = tack_quality_info(c->quality);
    uint8_t notes[8];
    int n = 0;
    for (uint8_t i = 0; i < q->count && n < 8; i++) {
        int note = 48 + c->root + q->intervals[i];
        while (note > 76) note -= 12;
        notes[n++] = (uint8_t)note;
    }
    while (n < 4) {
        notes[n] = (uint8_t)(notes[n - 1] + 12);
        n++;
    }
    /* Rootless comp shape: drop the lowest root, keep four upper voices. */
    int start = n > 4 ? 1 : 0;
    for (int i = 0; i < 4; i++) out[i] = notes[(start + i) % n];
    for (int i = 1; i < 4; i++) {
        uint8_t v = out[i];
        int j = i - 1;
        while (j >= 0 && out[j] > v) { out[j + 1] = out[j]; j--; }
        out[j + 1] = v;
    }
    return 4;
}

int tack_cantopop_hook_note(const TackState *s, uint8_t bar, uint8_t step) {
    if (!s || s->mode != TACK_MODE_CANTOPOP) return -1;
    static const uint8_t motif_even[3][2] = { {0,2}, {6,1}, {12,3} };
    static const uint8_t motif_odd[2][2] = { {0,1}, {8,3} };
    uint8_t note[4];
    const TackChord *c = &s->chords[(bar / 2) % TACK_SLOTS];
    if (tack_chord_notes(c, note) < 4) return -1;
    if (bar % 2 == 0) {
        for (int i = 0; i < 3; i++) if (motif_even[i][0] == step) return note[motif_even[i][1]] + 12;
    } else {
        for (int i = 0; i < 2; i++) if (motif_odd[i][0] == step) return note[motif_odd[i][1]] + 12;
    }
    return -1;
}
