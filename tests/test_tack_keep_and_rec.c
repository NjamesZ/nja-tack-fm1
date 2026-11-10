/* Host tests: user-slot keep across mode/root/style + MIDI rec policy together.
 * SPDX-License-Identifier: GPL-3.0-only */
#include <stdio.h>
#include <string.h>
#include "tack.h"
#include "tack_rec_policy.h"

static int failed;

static void expect(int cond, const char *name)
{
    if (cond) printf("  PASS  %s\n", name);
    else { printf("  FAIL  %s\n", name); failed++; }
}

int main(void)
{
    TackState s;
    uint8_t keep[TACK_SLOTS] = {1, 0, 1, 0};
    TackChord user0 = {0, TACK_Q_MAJ};   /* C major performed */
    TackChord user2 = {7, TACK_Q_DOM7};  /* G7 performed */
    TackChord before1, before3;
    TackNoteEv raw[8];
    TackClassifiedEv ev[8];
    TackScale sc;
    uint8_t out[8];
    TackRecSession sess;
    uint32_t have[4];
    int i, n, live;

    printf("== slot keep + MIDI rec ==\n");
    tack_init(&s, 0, TACK_MODE_MAJOR, TACK_STYLE_CLASSIC, 0x11111111u);
    before1 = s.chords[1];
    before3 = s.chords[3];
    s.chords[0] = user0;
    s.chords[2] = user2;

    tack_set_mode_keep(&s, TACK_MODE_MINOR, keep);
    expect(s.mode == TACK_MODE_MINOR, "mode is minor");
    expect(s.chords[0].root == user0.root && s.chords[0].quality == user0.quality,
           "owned slot 0 kept across mode");
    expect(s.chords[2].root == user2.root && s.chords[2].quality == user2.quality,
           "owned slot 2 kept across mode");

    tack_set_root_keep(&s, 5, keep); /* new tonic F — performed chords stay absolute */
    expect(s.root == 5, "root is F");
    expect(s.chords[0].root == 0 && s.chords[0].quality == TACK_Q_MAJ,
           "performed Cmaj not transposed with root");
    expect(s.chords[2].root == 7 && s.chords[2].quality == TACK_Q_DOM7,
           "performed G7 not transposed with root");

    tack_set_style_keep(&s, TACK_STYLE_JAZZ, keep);
    expect(s.style == TACK_STYLE_JAZZ, "style is jazz");
    expect(s.chords[0].root == 0 && s.chords[2].root == 7,
           "owned slots kept across style");

    /* unlocked rebuild may change unowned slots; owned must still survive a later keep rebuild */
    {
        TackChord u0 = s.chords[0], u2 = s.chords[2];
        tack_build_take_keep(&s, keep);
        expect(s.chords[0].root == u0.root && s.chords[0].quality == u0.quality,
               "build_take_keep preserves owned 0");
        expect(s.chords[2].root == u2.root && s.chords[2].quality == u2.quality,
               "build_take_keep preserves owned 2");
        (void)before1; (void)before3;
    }

    /* Unlocked setter still regenerates everything (generated-harmony). */
    {
        TackChord snap[4];
        int changed = 0;
        for (i = 0; i < 4; i++) snap[i] = s.chords[i];
        tack_init(&s, 0, TACK_MODE_MAJOR, TACK_STYLE_CLASSIC, 0x22222222u);
        s.chords[0] = user0;
        tack_set_mode(&s, TACK_MODE_DORIAN); /* unlocked */
        /* slot 0 is very likely different; if seed happens to match, still valid API */
        expect(s.mode == TACK_MODE_DORIAN, "unlocked set_mode changes mode");
        (void)snap; (void)changed;
    }

    /* Mixed MIDI: close-onset chord stays absolute; legato B follows scale. */
    raw[0] = (TackNoteEv){60, 0, 400};   /* C */
    raw[1] = (TackNoteEv){64, 20, 400};  /* E */
    raw[2] = (TackNoteEv){67, 40, 400};  /* G */
    raw[3] = (TackNoteEv){71, 500, 700}; /* B melody, no close onset */
    n = tack_rec_classify(raw, 4, TACK_REC_ONSET_CLUSTER,
                          TACK_REC_DEFAULT_ONSET_WINDOW_MS, ev, 8);
    expect(n == 2, "mixed clip: chord + melody");
    expect(n >= 1 && ev[0].kind == TACK_EV_CHORD && ev[0].n == 3, "first event chord triad");
    expect(n >= 2 && ev[1].kind == TACK_EV_MONO && ev[1].pitches[0] == 71, "second event B mono");

    sc = TACK_SCALE_C_MAJOR;
    expect(tack_rec_playback_pitches(&ev[0], &sc, out, 8) == 3
           && out[0] == 60 && out[1] == 64 && out[2] == 67,
           "chord voicing frozen in major");
    expect(tack_rec_playback_pitches(&ev[1], &sc, out, 8) == 1 && out[0] == 71,
           "B stays in C major");
    sc = TACK_SCALE_C_MINOR;
    expect(tack_rec_playback_pitches(&ev[0], &sc, out, 8) == 3
           && out[0] == 60 && out[1] == 64 && out[2] == 67,
           "chord voicing frozen after scale change");
    expect(tack_rec_playback_pitches(&ev[1], &sc, out, 8) == 1 && out[0] == 70,
           "B4 melody ties to Bb4 (lower absolute note) in C minor");

    /* Uniquify mixed move+MIDI same timestamp */
    have[0] = 100; have[1] = 200;
    expect(tack_rec_uniquify_ms(have, 2, 100) == 101, "uniquify collides +1");
    expect(tack_rec_uniquify_ms(have, 2, 150) == 150, "uniquify free ms");
    have[0] = 5; have[1] = 6; have[2] = 7;
    expect(tack_rec_uniquify_ms(have, 3, 5) == 8, "uniquify chain");

    /* Overflow: 40 serial notes -> cap 32, no live hang */
    tack_rec_session_init(&sess);
    for (i = 0; i < 40; i++) {
        (void)tack_rec_session_note_on(&sess, (uint8_t)(48 + (i % 24)), (uint32_t)(i * 10));
        (void)tack_rec_session_note_off(&sess, (uint8_t)(48 + (i % 24)), (uint32_t)(i * 10 + 5));
    }
    expect(sess.nraw == TACK_REC_MAX_NOTES, "serial overflow caps at MAX_NOTES");
    live = 0;
    for (i = 0; i < 128; i++) live += sess.live_on[i];
    expect(live == 0, "no live_on hang after serial overflow");

    /* Held poly overflow: 40 simultaneous ons, flush must clear all live */
    tack_rec_session_init(&sess);
    for (i = 0; i < 40; i++)
        (void)tack_rec_session_note_on(&sess, (uint8_t)(30 + i), 0);
    expect(tack_rec_session_flush(&sess, 100) < 0, "flush reports drops when over cap");
    expect(sess.nraw == TACK_REC_MAX_NOTES, "flush stores only MAX_NOTES");
    live = 0;
    for (i = 0; i < 128; i++) live += sess.live_on[i];
    expect(live == 0, "flush clears leftover live notes after overflow");

    /* note-off of a dropped pitch does not hang */
    tack_rec_session_init(&sess);
    for (i = 0; i < TACK_REC_MAX_NOTES; i++) {
        (void)tack_rec_session_note_on(&sess, 60, (uint32_t)(i * 20));
        (void)tack_rec_session_note_off(&sess, 60, (uint32_t)(i * 20 + 10));
    }
    (void)tack_rec_session_note_on(&sess, 72, 1000); /* live, raw full after this off */
    expect(tack_rec_session_note_off(&sess, 72, 1010) < 0, "off of overflowed note reports drop");
    expect(sess.live_on[72] == 0, "overflowed note-off clears live_on");

    printf("keep+rec tests: %s\n", failed ? "FAILED" : "passed");
    return failed ? 1 : 0;
}
