#include <stdio.h>
#include <string.h>
#include "tack_rec_policy.h"

static int g_fail = 0;
static int g_pass = 0;

static void check(const char *id, int cond)
{
    if (cond) { g_pass++; printf("  PASS  %s\n", id); }
    else { g_fail++; printf("  FAIL  %s\n", id); }
}

static int pitches_eq(const uint8_t *a, const uint8_t *b, int n)
{
    int i;
    for (i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

int main(void)
{
    TackClassifiedEv evs[16];
    uint8_t out[8];
    int n;

    /* R1: monophonic C-D-E in C major; playback in C minor retunes E->Eb. */
    {
        TackNoteEv notes[] = {
            {60, 0, 200}, {62, 250, 450}, {64, 500, 700}
        };
        n = tack_rec_classify(notes, 3, TACK_REC_ONSET_CLUSTER, 80, evs, 16);
        check("R1 classify count", n == 3);
        check("R1 all mono", n == 3 && evs[0].kind == TACK_EV_MONO
              && evs[1].kind == TACK_EV_MONO && evs[2].kind == TACK_EV_MONO);
        tack_rec_playback_pitches(&evs[0], &TACK_SCALE_C_MINOR, out, 8);
        check("R1 C stays", out[0] == 60);
        tack_rec_playback_pitches(&evs[1], &TACK_SCALE_C_MINOR, out, 8);
        check("R1 D stays", out[0] == 62);
        tack_rec_playback_pitches(&evs[2], &TACK_SCALE_C_MINOR, out, 8);
        check("R1 E->Eb", out[0] == 63);
        tack_rec_playback_pitches(&evs[2], &TACK_SCALE_C_MAJOR, out, 8);
        check("R1 E stays in major", out[0] == 64);
    }

    /* R2: block triad C-E-G simultaneous onset — chord, pitches frozen in minor. */
    {
        TackNoteEv notes[] = {
            {60, 0, 400}, {64, 0, 400}, {67, 0, 400}
        };
        uint8_t expect[3] = {60, 64, 67};
        n = tack_rec_classify(notes, 3, TACK_REC_ONSET_CLUSTER, 80, evs, 16);
        check("R2 one event", n == 1 && evs[0].kind == TACK_EV_CHORD && evs[0].n == 3);
        tack_rec_playback_pitches(&evs[0], &TACK_SCALE_C_MINOR, out, 8);
        check("R2 pitches preserved", pitches_eq(out, expect, 3));
    }

    /* R3: 30–80 ms roll still a chord under onset-cluster. */
    {
        TackNoteEv notes[] = {
            {60, 0, 400}, {64, 40, 400}, {67, 80, 400}
        };
        uint8_t expect[3] = {60, 64, 67};
        n = tack_rec_classify(notes, 3, TACK_REC_ONSET_CLUSTER, 80, evs, 16);
        check("R3 rolled chord", n == 1 && evs[0].kind == TACK_EV_CHORD && evs[0].n == 3);
        tack_rec_playback_pitches(&evs[0], &TACK_SCALE_C_MINOR, out, 8);
        check("R3 pitches preserved", pitches_eq(out, expect, 3));
    }

    /* R4: legato two-note overlap, sequential intent.
     * Authorized ONSET_CLUSTER: second onset 200ms later → two MONO (scale-adapt).
     * SUSTAIN_OVERLAP contrast: one CHORD — kept as a negative test only. */
    {
        TackNoteEv notes[] = {
            {60, 0, 350}, {64, 200, 500}
        };
        n = tack_rec_classify(notes, 2, TACK_REC_ONSET_CLUSTER, 80, evs, 16);
        check("R4 cluster = two mono", n == 2 && evs[0].kind == TACK_EV_MONO
              && evs[1].kind == TACK_EV_MONO);
        n = tack_rec_classify(notes, 2, TACK_REC_SUSTAIN_OVERLAP, 80, evs, 16);
        check("R4 sustain = one chord", n == 1 && evs[0].kind == TACK_EV_CHORD && evs[0].n == 2);
    }

    /* R5: pedal C held under melody E-D — onset-cluster keeps pedal mono
     * and melody mono (no false whole-clip chord from pitch count). */
    {
        TackNoteEv notes[] = {
            {48, 0, 2000}, {64, 400, 600}, {62, 700, 900}
        };
        n = tack_rec_classify(notes, 3, TACK_REC_ONSET_CLUSTER, 80, evs, 16);
        check("R5 three events not one chord", n == 3);
        check("R5 no clip-wide chord", n == 3 && evs[0].kind == TACK_EV_MONO
              && evs[1].kind == TACK_EV_MONO && evs[2].kind == TACK_EV_MONO);
        n = tack_rec_classify(notes, 3, TACK_REC_SUSTAIN_OVERLAP, 80, evs, 16);
        check("R5 sustain treats pedal+line as chord(s)", n >= 1 && evs[0].kind == TACK_EV_CHORD);
    }

    /* R6: repeated single pitch class — not a chord. */
    {
        TackNoteEv notes[] = {
            {60, 0, 150}, {60, 200, 350}, {60, 400, 550}
        };
        n = tack_rec_classify(notes, 3, TACK_REC_ONSET_CLUSTER, 80, evs, 16);
        check("R6 three mono", n == 3 && evs[0].kind == TACK_EV_MONO
              && evs[1].kind == TACK_EV_MONO && evs[2].kind == TACK_EV_MONO);
    }

    /* R7: serial multi-pitch, no overlap — scale-adapt as line, not a chord. */
    {
        TackNoteEv notes[] = {
            {60, 0, 100}, {64, 150, 250}, {67, 300, 400}, {71, 450, 550}
        };
        n = tack_rec_classify(notes, 4, TACK_REC_ONSET_CLUSTER, 80, evs, 16);
        check("R7 four mono (not chord from pitch count)", n == 4
              && evs[0].kind == TACK_EV_MONO && evs[3].kind == TACK_EV_MONO);
        tack_rec_playback_pitches(&evs[1], &TACK_SCALE_C_MINOR, out, 8);
        check("R7 serial E adapts", out[0] == 63);
    }

    /* R8: recorded chord then mode/root change — intervals preserved (absolute). */
    {
        TackNoteEv notes[] = {
            {60, 0, 400}, {64, 5, 400}, {67, 10, 400}
        };
        uint8_t expect[3] = {60, 64, 67};
        n = tack_rec_classify(notes, 3, TACK_REC_ONSET_CLUSTER, 80, evs, 16);
        tack_rec_playback_pitches(&evs[0], &TACK_SCALE_C_MINOR, out, 8);
        check("R8 absolute voicing kept", n == 1 && pitches_eq(out, expect, 3));
    }

    /* R9: empty / single note / degenerate off==on rejected. */
    {
        TackNoteEv one[] = { {60, 0, 100} };
        TackNoteEv bad[] = { {60, 10, 10} };
        n = tack_rec_classify(NULL, 0, TACK_REC_ONSET_CLUSTER, 80, evs, 16);
        check("R9 empty", n == 0);
        n = tack_rec_classify(one, 1, TACK_REC_ONSET_CLUSTER, 80, evs, 16);
        check("R9 single mono", n == 1 && evs[0].kind == TACK_EV_MONO && evs[0].pitches[0] == 60);
        n = tack_rec_classify(bad, 1, TACK_REC_ONSET_CLUSTER, 80, evs, 16);
        check("R9 reject zero duration", n == -1);
    }

    /* R10: duplicate note-ons (same pitch, sequential) deterministic mono. */
    {
        TackNoteEv notes[] = {
            {60, 0, 80}, {60, 80, 160}
        };
        n = tack_rec_classify(notes, 2, TACK_REC_ONSET_CLUSTER, 80, evs, 16);
        check("R10 two sequential same pitch", n == 2 && evs[0].kind == TACK_EV_MONO
              && evs[1].kind == TACK_EV_MONO);
    }

    /* Mixed clip: melody then block chord — per-event policy. */
    {
        TackNoteEv notes[] = {
            {72, 0, 120}, {74, 140, 260},
            {60, 400, 800}, {64, 405, 800}, {67, 410, 800}
        };
        n = tack_rec_classify(notes, 5, TACK_REC_ONSET_CLUSTER, 80, evs, 16);
        check("MIX count", n == 3);
        check("MIX melody+chord", n == 3 && evs[0].kind == TACK_EV_MONO
              && evs[1].kind == TACK_EV_MONO && evs[2].kind == TACK_EV_CHORD && evs[2].n == 3);
        tack_rec_playback_pitches(&evs[0], &TACK_SCALE_C_MINOR, out, 8);
        check("MIX melody C5 stays", out[0] == 72);
        {
            uint8_t expect[3] = {60, 64, 67};
            tack_rec_playback_pitches(&evs[2], &TACK_SCALE_C_MINOR, out, 8);
            check("MIX chord frozen", pitches_eq(out, expect, 3));
        }
    }

    /* Firmware model: single key 0..5 on HOME = move, not capture.
     * 3+ held keys = capture. Serial never captures. */
    {
        check("FW single key move", tack_rec_fw_would_capture_chord(1u << 2, 1u << 2, 1) == 0);
        check("FW two keys no capture", tack_rec_fw_would_capture_chord((1u << 0) | (1u << 4), 1u << 4, 1) == 0);
        check("FW triad captures", tack_rec_fw_would_capture_chord(0x15u, 0x10u, 1) == 1);
        {
            uint8_t triad[3] = {60, 64, 67};
            uint8_t unk[3] = {60, 61, 66}; /* C, C#, F# — firmware CLUSTER */
            check("FW quality maj", tack_rec_fw_kb_quality(triad, 3) == 1);
            check("FW quality cluster fallback", tack_rec_fw_kb_quality(unk, 3) == 23);
        }
    }

    /* BOUND: 80 ms inclusive vs 81 ms exclusive, vs first onset not chained. */
    {
        TackNoteEv at80[] = {
            {60, 0, 400}, {64, 80, 400}
        };
        TackNoteEv at81[] = {
            {60, 0, 400}, {64, 81, 400}
        };
        n = tack_rec_classify(at80, 2, TACK_REC_ONSET_CLUSTER, 80, evs, 16);
        check("BOUND 80ms is chord", n == 1 && evs[0].kind == TACK_EV_CHORD);
        n = tack_rec_classify(at81, 2, TACK_REC_ONSET_CLUSTER, 80, evs, 16);
        check("BOUND 81ms is legato mono", n == 2 && evs[0].kind == TACK_EV_MONO
              && evs[1].kind == TACK_EV_MONO);
    }

    /* CHAIN: onsets 0,70,140 all overlapping — must NOT become one triad.
     * Pairwise hops are 70 ms each; distance from first press to last is 140. */
    {
        TackNoteEv notes[] = {
            {60, 0, 500}, {64, 70, 500}, {67, 140, 500}
        };
        n = tack_rec_classify(notes, 3, TACK_REC_ONSET_CLUSTER, 80, evs, 16);
        check("CHAIN not one chord", n == 2);
        check("CHAIN first is C+E dyad", n == 2 && evs[0].kind == TACK_EV_CHORD
              && evs[0].n == 2 && evs[0].pitches[0] == 60 && evs[0].pitches[1] == 64);
        check("CHAIN last G is mono", n == 2 && evs[1].kind == TACK_EV_MONO
              && evs[1].n == 1 && evs[1].pitches[0] == 67);
        {
            TackPlaybackMap map;
            tack_rec_map_event(&evs[0], &TACK_SCALE_C_MINOR, &map);
            check("CHAIN dyad raw==mapped", map.raw[0] == 60 && map.mapped[0] == 60
                  && map.raw[1] == 64 && map.mapped[1] == 64);
            tack_rec_map_event(&evs[1], &TACK_SCALE_C_MINOR, &map);
            check("CHAIN G adapts to minor", map.raw[0] == 67 && map.mapped[0] == 67);
        }
    }

    /* ROLL4: 0/25/50/75 within 80 of first = one chord; 0/30/60/90 splits. */
    {
        TackNoteEv fast[] = {
            {60, 0, 400}, {64, 25, 400}, {67, 50, 400}, {71, 75, 400}
        };
        TackNoteEv slow[] = {
            {60, 0, 400}, {64, 30, 400}, {67, 60, 400}, {71, 90, 400}
        };
        uint8_t expect[4] = {60, 64, 67, 71};
        n = tack_rec_classify(fast, 4, TACK_REC_ONSET_CLUSTER, 80, evs, 16);
        check("ROLL4 fast one chord", n == 1 && evs[0].kind == TACK_EV_CHORD && evs[0].n == 4);
        check("ROLL4 voicing kept", n == 1 && pitches_eq(evs[0].pitches, expect, 4));
        n = tack_rec_classify(slow, 4, TACK_REC_ONSET_CLUSTER, 80, evs, 16);
        check("ROLL4 slow splits last", n == 2 && evs[0].kind == TACK_EV_CHORD && evs[0].n == 3
              && evs[1].kind == TACK_EV_MONO && evs[1].pitches[0] == 71);
    }

    /* NOTEOFF: exclusive endpoints do not overlap (serial); 1 ms overlap does. */
    {
        TackNoteEv touch[] = { {60, 0, 50}, {64, 50, 100} };
        TackNoteEv ov[] = { {60, 0, 51}, {64, 50, 100} };
        n = tack_rec_classify(touch, 2, TACK_REC_ONSET_CLUSTER, 80, evs, 16);
        check("NOTEOFF touching = serial mono", n == 2 && evs[0].kind == TACK_EV_MONO
              && evs[1].kind == TACK_EV_MONO);
        n = tack_rec_classify(ov, 2, TACK_REC_ONSET_CLUSTER, 80, evs, 16);
        check("NOTEOFF 1ms overlap + close onset = chord", n == 1 && evs[0].kind == TACK_EV_CHORD);
    }

    /* SESSION: note-on/off bookkeeping → same classify as raw list.
     * Unmatched note-off ignored; flush closes hanging ons; retrigger splits. */
    {
        TackRecSession sess;
        TackPlaybackMap map;
        tack_rec_session_init(&sess);
        check("SESS note-off unknown ok", tack_rec_session_note_off(&sess, 60, 10) == 0
              && sess.nraw == 0);
        tack_rec_session_note_on(&sess, 60, 0);
        tack_rec_session_note_on(&sess, 64, 0);
        tack_rec_session_note_on(&sess, 67, 0);
        tack_rec_session_note_off(&sess, 60, 400);
        tack_rec_session_note_off(&sess, 64, 400);
        tack_rec_session_note_off(&sess, 67, 400);
        n = tack_rec_classify(sess.raw, sess.nraw, TACK_REC_ONSET_CLUSTER, 80, evs, 16);
        check("SESS block triad", n == 1 && evs[0].kind == TACK_EV_CHORD && evs[0].n == 3);
        tack_rec_map_event(&evs[0], &TACK_SCALE_C_MINOR, &map);
        check("SESS triad raw==mapped", map.kind == TACK_EV_CHORD
              && map.raw[0] == 60 && map.mapped[0] == 60
              && map.raw[1] == 64 && map.mapped[1] == 64
              && map.raw[2] == 67 && map.mapped[2] == 67);

        tack_rec_session_init(&sess);
        tack_rec_session_note_on(&sess, 60, 0);
        tack_rec_session_note_off(&sess, 60, 200);
        tack_rec_session_note_on(&sess, 64, 250);
        tack_rec_session_note_off(&sess, 64, 450);
        tack_rec_session_note_on(&sess, 67, 500);
        tack_rec_session_flush(&sess, 700);
        n = tack_rec_classify(sess.raw, sess.nraw, TACK_REC_ONSET_CLUSTER, 80, evs, 16);
        check("SESS serial three raw", sess.nraw == 3 && n == 3
              && evs[0].kind == TACK_EV_MONO && evs[2].kind == TACK_EV_MONO);
        tack_rec_map_event(&evs[1], &TACK_SCALE_C_MINOR, &map);
        check("SESS serial E maps to Eb", map.raw[0] == 64 && map.mapped[0] == 63);

        tack_rec_session_init(&sess);
        tack_rec_session_note_on(&sess, 60, 0);
        tack_rec_session_note_on(&sess, 64, 200); /* legato, far onset */
        tack_rec_session_note_off(&sess, 60, 350);
        tack_rec_session_note_off(&sess, 64, 500);
        n = tack_rec_classify(sess.raw, sess.nraw, TACK_REC_ONSET_CLUSTER, 80, evs, 16);
        check("SESS legato two mono", sess.nraw == 2 && n == 2
              && evs[0].kind == TACK_EV_MONO && evs[1].kind == TACK_EV_MONO);
        tack_rec_map_event(&evs[1], &TACK_SCALE_C_MINOR, &map);
        check("SESS legato E follows scale", map.raw[0] == 64 && map.mapped[0] == 63);

        tack_rec_session_init(&sess);
        tack_rec_session_note_on(&sess, 64, 0);
        tack_rec_session_note_on(&sess, 64, 100); /* retrigger */
        tack_rec_session_note_off(&sess, 64, 200);
        check("SESS retrigger two raw", sess.nraw == 2 && sess.raw[0].onset_ms == 0
              && sess.raw[0].off_ms == 100 && sess.raw[1].onset_ms == 100
              && sess.raw[1].off_ms == 200);
    }

    /* Unordered input: late array first — still groups from earliest onset. */
    {
        TackNoteEv notes[] = {
            {67, 80, 400}, {60, 0, 400}, {64, 40, 400}
        };
        n = tack_rec_classify(notes, 3, TACK_REC_ONSET_CLUSTER, 80, evs, 16);
        check("ORDER independent roll", n == 1 && evs[0].kind == TACK_EV_CHORD && evs[0].n == 3);
    }

    printf("\npolicy tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
