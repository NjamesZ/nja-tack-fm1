#include "tack_rec_policy.h"

const TackScale TACK_SCALE_C_MAJOR = { 0, 7, { 0, 2, 4, 5, 7, 9, 11 } };
const TackScale TACK_SCALE_C_MINOR = { 0, 7, { 0, 2, 3, 5, 7, 8, 10 } };

#ifndef TACK_H
enum { TACK_Q_CLUSTER = 23 };
#endif

static void sort_u8(uint8_t *a, int n)
{
    int i, j;
    for (i = 1; i < n; i++) {
        uint8_t v = a[i];
        j = i - 1;
        while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; j--; }
        a[j + 1] = v;
    }
}

static int overlap(const TackNoteEv *a, const TackNoteEv *b)
{
    uint32_t as = a->onset_ms, ae = a->off_ms;
    uint32_t bs = b->onset_ms, be = b->off_ms;
    return as < be && bs < ae;
}

static uint32_t abs_diff_u32(uint32_t a, uint32_t b)
{
    return (a >= b) ? (a - b) : (b - a);
}

uint8_t tack_rec_adapt_pitch(uint8_t pitch, const TackScale *scale)
{
    int candidate, best = pitch, distance = 128, i;
    if (!scale || !scale->interval_count || scale->interval_count > 12u) return pitch;
    /* Search absolute MIDI notes, not circular pitch classes reconstructed in
     * the old octave. Ascending search gives lower absolute note on a tie. */
    for (candidate = 0; candidate <= 127; candidate++) {
        int d = candidate > pitch ? candidate - pitch : pitch - candidate;
        if (d >= distance) continue;
        for (i = 0; i < scale->interval_count; i++) {
            if (candidate % 12 == (scale->tonic + scale->intervals[i]) % 12) {
                best = candidate; distance = d; break;
            }
        }
    }
    return (uint8_t)best;
}

int tack_rec_playback_pitches(const TackClassifiedEv *ev, const TackScale *scale,
                              uint8_t *out, int out_cap)
{
    int i;
    if (!ev || !out || out_cap < ev->n) return -1;
    for (i = 0; i < ev->n; i++) {
        if (ev->kind == TACK_EV_CHORD)
            out[i] = ev->pitches[i];
        else
            out[i] = tack_rec_adapt_pitch(ev->pitches[i], scale);
    }
    return ev->n;
}

uint8_t tack_rec_fw_kb_quality(const uint8_t *n, uint8_t cnt)
{
    uint16_t mask = 1u;
    uint8_t i;
    if (!n || cnt < 1u) return (uint8_t)TACK_Q_CLUSTER;
    for (i = 1; i < cnt; i++)
        mask |= (uint16_t)(1u << ((n[i] - n[0]) % 12u));
    if (cnt >= 4u) {
        if (mask == ((1u << 0) | (1u << 4) | (1u << 7) | (1u << 11))) return 5;  /* MAJ7 */
        if (mask == ((1u << 0) | (1u << 4) | (1u << 7) | (1u << 10))) return 7;  /* DOM7 */
        if (mask == ((1u << 0) | (1u << 3) | (1u << 7) | (1u << 10))) return 3;  /* MIN7 */
        if (mask == ((1u << 0) | (1u << 3) | (1u << 6) | (1u << 10))) return 12; /* M7B5 */
        if (mask == ((1u << 0) | (1u << 3) | (1u << 6) | (1u << 9))) return 20;  /* DIM7 */
    }
    if (mask == ((1u << 0) | (1u << 3) | (1u << 7))) return 0; /* MIN */
    if (mask == ((1u << 0) | (1u << 4) | (1u << 7))) return 1; /* MAJ */
    if (mask == ((1u << 0) | (1u << 3) | (1u << 6))) return 2; /* DIM */
    if (mask == ((1u << 0) | (1u << 4) | (1u << 8))) return 19; /* AUG */
    if (mask == ((1u << 0) | (1u << 5) | (1u << 7))) return 10; /* SUS4 */
    if (mask == ((1u << 0) | (1u << 2) | (1u << 7))) return 9;  /* SUS2 */
    return (uint8_t)TACK_Q_CLUSTER;
}

int tack_rec_fw_would_capture_chord(uint32_t held_notes, uint32_t rising,
                                    int page_is_home_or_loop)
{
    int pop = 0;
    uint32_t bits = held_notes;
    while (bits) { pop++; bits &= bits - 1u; }
    if (page_is_home_or_loop && rising && (rising & (rising - 1u)) == 0u && pop == 1) {
        uint32_t bit = 0;
        uint32_t r = rising;
        while (r > 1u) { r >>= 1u; bit++; }
        if (bit < 6u) return 0; /* MOVE pick, not a recorded pitch */
    }
    return pop >= 3 ? 1 : 0;
}

int tack_rec_map_event(const TackClassifiedEv *ev, const TackScale *scale,
                       TackPlaybackMap *out)
{
    int i;
    if (!ev || !out) return -1;
    out->kind = ev->kind;
    out->n = ev->n;
    for (i = 0; i < ev->n; i++) {
        out->raw[i] = ev->pitches[i];
        if (ev->kind == TACK_EV_CHORD)
            out->mapped[i] = ev->pitches[i];
        else
            out->mapped[i] = tack_rec_adapt_pitch(ev->pitches[i], scale);
    }
    return ev->n;
}

void tack_rec_session_init(TackRecSession *s)
{
    int i;
    if (!s) return;
    s->nraw = 0;
    s->active_n = 0;
    s->dropped = 0;
    for (i = 0; i < 128; i++) {
        s->live_on[i] = 0;
        s->live_onset[i] = 0;
    }
}

static int session_close(TackRecSession *s, uint8_t pitch, uint32_t t_ms)
{
    uint32_t on, off;
    if (!s || pitch > 127u || !s->live_on[pitch]) return 0;
    on = s->live_onset[pitch];
    off = t_ms;
    if (off <= on) off = on + 1u;
    if (s->nraw >= TACK_REC_MAX_NOTES) {
        /* Drop the event but always release live_on — no note-off hang. */
        s->live_on[pitch] = 0;
        return -1;
    }
    s->raw[s->nraw].pitch = pitch;
    s->raw[s->nraw].onset_ms = on;
    s->raw[s->nraw].off_ms = off;
    s->nraw++;
    s->live_on[pitch] = 0;
    return 0;
}

int tack_rec_session_note_on(TackRecSession *s, uint8_t pitch, uint32_t t_ms)
{
    if (!s || pitch > 127u) return -1;
    if (s->live_on[pitch]) {
        if (session_close(s, pitch, t_ms) < 0) return -1;
    }
    s->live_on[pitch] = 1;
    s->live_onset[pitch] = t_ms;
    return 0;
}

int tack_rec_session_note_off(TackRecSession *s, uint8_t pitch, uint32_t t_ms)
{
    if (!s || pitch > 127u) return -1;
    return session_close(s, pitch, t_ms);
}

int tack_rec_session_flush(TackRecSession *s, uint32_t t_ms)
{
    int p, rc = 0;
    if (!s) return -1;
    for (p = 0; p < 128; p++) {
        if (s->live_on[p]) {
            /* Keep going so a full buffer cannot leave later pitches hanging. */
            if (session_close(s, (uint8_t)p, t_ms) < 0) rc = -1;
        }
    }
    return rc;
}

uint32_t tack_rec_uniquify_ms(const uint32_t *have, int nhave, uint32_t ms)
{
    int i, hit;
    if (nhave < 0) nhave = 0;
    do {
        hit = 0;
        for (i = 0; i < nhave; i++) {
            if (have && have[i] == ms) { ms++; hit = 1; break; }
        }
    } while (hit);
    return ms;
}

int tack_rec_classify(const TackNoteEv *notes, int nnotes,
                      TackRecPolicy policy, uint32_t onset_window_ms,
                      TackClassifiedEv *out, int out_cap)
{
    int used[TACK_REC_MAX_NOTES];
    int i, j, nout = 0;
    if (nnotes < 0 || (nnotes > 0 && !notes)) return -1;
    if (nnotes > TACK_REC_MAX_NOTES) return -1;
    if (out_cap < 0 || (out_cap > 0 && !out)) return -1;
    for (i = 0; i < nnotes; i++) {
        if (notes[i].off_ms <= notes[i].onset_ms) return -1;
        used[i] = 0;
    }
    if (onset_window_ms == 0) onset_window_ms = TACK_REC_DEFAULT_ONSET_WINDOW_MS;

    /* Seed each cluster from the earliest unused onset so grouping does not
     * depend on array order, and the origin is always the first press. */
    for (;;) {
        int seed = -1;
        int members[TACK_REC_MAX_VOICES];
        int nm = 0;
        uint32_t cluster_origin;
        for (i = 0; i < nnotes; i++) {
            if (used[i]) continue;
            if (seed < 0
                || notes[i].onset_ms < notes[seed].onset_ms
                || (notes[i].onset_ms == notes[seed].onset_ms && i < seed))
                seed = i;
        }
        if (seed < 0) break;

        members[nm++] = seed;
        used[seed] = 1;
        cluster_origin = notes[seed].onset_ms;

        /* Grow only with notes that (1) actually overlap some member and
         * (2) under ONSET_CLUSTER, start within the window of the FIRST
         * onset — never of a later member (no transitive 80+80+80 chains). */
        {
            int grew = 1;
            while (grew) {
                grew = 0;
                for (j = 0; j < nnotes; j++) {
                    int m, hits = 0;
                    if (used[j] || nm >= TACK_REC_MAX_VOICES) continue;
                    for (m = 0; m < nm; m++) {
                        if (overlap(&notes[j], &notes[members[m]])) { hits = 1; break; }
                    }
                    if (!hits) continue;
                    if (policy == TACK_REC_ONSET_CLUSTER) {
                        if (abs_diff_u32(notes[j].onset_ms, cluster_origin) > onset_window_ms)
                            continue;
                    }
                    members[nm++] = j;
                    used[j] = 1;
                    grew = 1;
                }
            }
        }

        if (nout >= out_cap) return -1;
        {
            TackClassifiedEv *ev = &out[nout++];
            uint32_t on = notes[members[0]].onset_ms;
            uint32_t off = notes[members[0]].off_ms;
            ev->members = 0;
            ev->n = (uint8_t)nm;
            for (j = 0; j < nm; j++) {
                ev->members |= (uint32_t)1u << members[j];
                ev->pitches[j] = notes[members[j]].pitch;
                if (notes[members[j]].onset_ms < on) on = notes[members[j]].onset_ms;
                if (notes[members[j]].off_ms > off) off = notes[members[j]].off_ms;
            }
            sort_u8(ev->pitches, nm);
            ev->onset_ms = on;
            ev->off_ms = off;
            ev->kind = (nm >= (int)TACK_REC_DEFAULT_MIN_CHORD_NOTES)
                ? TACK_EV_CHORD : TACK_EV_MONO;
        }
    }
    return nout;
}

/* Firmware uses compact channel/pitch ownership. Legacy one-channel wrappers
 * remain for the standalone policy tests; the firmware never mixes APIs. */
static int tack_rec_ch_close(TackRecSession *s, int i, uint32_t ms)
{
    int k, rc = 0;
    uint8_t pitch = s->active_pitch[i];
    if (s->nraw >= TACK_REC_MAX_NOTES) { s->dropped = 1; rc = -1; }
    else {
        int n = s->nraw++;
        s->raw[n].pitch = pitch;
        s->raw[n].onset_ms = s->active_start[i];
        s->raw[n].off_ms = ms > s->active_start[i] ? ms : s->active_start[i] + 1u;
        s->raw_channel[n] = s->active_channel[i];
    }
    --s->active_n;
    s->active_pitch[i] = s->active_pitch[s->active_n];
    s->active_channel[i] = s->active_channel[s->active_n];
    s->active_start[i] = s->active_start[s->active_n];
    s->live_on[pitch] = 0;
    for (k = 0; k < s->active_n; k++) if (s->active_pitch[k] == pitch) s->live_on[pitch] = 1;
    return rc;
}
int tack_rec_session_channel(TackRecSession *s, uint8_t ch, uint8_t pitch,
                             uint8_t vel, uint32_t ms)
{
    int i;
    if (!s || ch > 15u || pitch > 127u) return -1;
    for (i = 0; i < s->active_n; i++)
        if (s->active_channel[i] == ch && s->active_pitch[i] == pitch) break;
    if (i < s->active_n) {
        if (vel && s->active_start[i] == ms) return 0; /* identical same-time callback */
        (void)tack_rec_ch_close(s, i, ms);
    } else if (!vel) return 0; /* unmatched release is harmless */
    if (!vel) return s->dropped ? -1 : 0;
    if (s->active_n + s->nraw >= TACK_REC_MAX_NOTES) {
        s->dropped = 1; return -1;
    }
    i = s->active_n++;
    s->active_channel[i] = ch; s->active_pitch[i] = pitch; s->active_start[i] = ms;
    s->live_on[pitch] = 1;
    return 0;
}
int tack_rec_session_channel_flush(TackRecSession *s, uint32_t ms)
{
    if (!s) return -1;
    while (s->active_n) (void)tack_rec_ch_close(s, s->active_n - 1, ms);
    return s->dropped ? -1 : 0;
}
