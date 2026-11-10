# Quick start

Repository: <https://github.com/NjamesZ/nja-tack-fm1>

Start at low output volume. Back up important projects and keep a recovery package.
Confirm the installed identity using `tools/fm1_install.py --info` before evaluating.
For this candidate the expected running identity is **FM-1_65262805** (6.28-rc5).

One controlled install of the exact package succeeded on 10 November 2026 at
11:32 HKT, and a post-reboot handshake reported that identity. Audio behaviour
and the wider install/recovery matrix are still open. See [test status](TESTING.md).

## Make harmony

- On HOME, keys **1–6** choose harmony moves and advance the target slot.
- K1 chooses a move; K2 (or SELECT/SEQ on HOME) commits it. K3 switches A/B;
  K4 changes mode in the normal HOME view. MORE/arranger views have other roles.
- SCL opens STYLE, EDIT opens LIBRARY; HOME returns from an edit page.
- Start/stop transport as usual. Do not infer successful audio from the screen.

## Record played MIDI

1. Connect a MIDI controller to the configured USB or TRS MIDI input. Disable any
   software MIDI-through/loopback route. Check normal MIDI sound/routing first.
2. From HOME press REC to open LOOP. This navigation does not by itself start a take.
3. Select REC with K1; turn K2 to arm (`ARMED`). Or use a short REC press on LOOP.
4. Play separate C–D–E notes, then an overlapping C–E–G chord whose onsets are
   within 80 ms. Hardware keys 1–6 still pick harmony moves during this recording.
5. Stop with the same REC action. The take is HELD, not automatically looping.
6. Choose LOOP with K1 and turn K2 to start/stop performed playback. Changing
   scale/root should affect melody, not the close-onset chord's absolute pitches.
7. K4 APPLY is an explicit alternative: it writes patterned harmony into four slots,
   not a duration-faithful export of what you played.
8. On LOOP hold REC for at least 600 ms to CLEAR. Check every held note releases.

Read [recording policy](RECORDING.md) before relying on storage, quantize, capacity
or export. A positive short trial is not an exhaustive hardware qualification.
