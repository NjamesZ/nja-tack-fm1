# Recording, chords and scale policy

## Policy in one pass

- Played notes follow the selected scale.
- Overlapping notes that begin within **80 ms of the first onset**, and that
  actually overlap, keep their literal pitches. The window is anchored on that
  first onset and does not chain from note to note.
- Legato, and later notes outside the window, follow the scale.
- Keys **1–6** pick harmony moves during recording. They are not piano keys.
- At most **four** concurrent recorded MIDI voices. If a take exceeds that limit,
  or the other capture limits below, the whole take is rejected.
- New timing, channel, and literal-voicing metadata are **RAM-only**.

## Intent and pitch

The recorder receives played pitches through the existing USB/TRS MIDI note path.
The six hardware keys retain harmony moves; no conflicting piano-key mode was added.

With the onset policy, overlapping notes whose onsets fall within **80 ms of the
cluster's earliest onset** form a performed chord. Later overlap/legato does not
extend that window transitively. Chord members keep exact absolute MIDI pitches,
including their voicing; mode/root/global generated-harmony octave changes must
not transpose them. Mono events follow the active scale/root via nearest absolute
MIDI-note quantization; ties go to the lower absolute note. Example: B4 (71) in
C minor becomes B-flat4 (70), not C4 (60).

Four-slot user-owned chords survive take rebuild, mode and style rebind. Unowned
slots continue generated-harmony behavior. Explicitly selecting a harmony move
replaces its slot's old literal metadata. RAM A/B and undo carry this ownership.

## Timing and mixed takes

LOOP retains individual raw MIDI onsets/releases and a span covering the final
release. A held note keeps the mapped pitch used at note-on until its note-off,
including across root/mode changes. It uses millisecond/audio-block scheduling,
not sample-exact timestamps. A completely missed very short note may be skipped
by a late block rather than left hanging.

Move tokens and played MIDI share the bounded capture timeline: move tokens steer
backing harmony while MIDI occupies the comp lane. APPLY is the existing four-slot
patterned-harmony writer, **not** a faithful performance export. Quantize/rhythm
controls act on the legacy harmony representation; they should not be understood
as a lossless MIDI editor. Use LOOP to evaluate performed duration/voicing.

## Capacity and ownership

- At most **four concurrent recorded MIDI voices**, within a shared eight-voice
  synthesis budget. Other sounding parts can affect available voices.
- 32 raw notes; 16 combined classified-note/move events; 64 queued MIDI messages.
- A capacity/voice overflow rejects the entire capture visibly as REC FULL at stop.
  It is not an accepted partial clip. Release external keys before starting again.
- Channel plus raw pitch identify input releases; mapped pitches are pinned for
  playback release. Simultaneous channels are not collapsed by pitch alone.
- The main loop owns capture sessions, fed by a bounded audio-ISR queue. Gate
  closure precedes finalize; rearm resets session/metadata. Long UI stalls can
  exhaust the queue, in which case rejection is intentional rather than silent.
- Internal LOOP goes straight to playback and is not recorded back as MIDI input.
  There is no pitch-only echo filter that guesses away real external releases.
  External MIDI loopback cannot be reliably identified without source tags: disable it.

## Storage and export warning

Raw timing, channel bookkeeping and literal-voicing metadata are **RAM-only**.
Legacy projects/performance saves and SysEx82 phrase JSON remain harmony-token
formats. Power loss/reload/export does not guarantee your exact performance returns.
A/B and undo are not durable backups. No persistence-format migration is included.

To pull an armed EXPORT snapshot, install `mido` and `python-rtmidi`, then run:

```sh
python3 tools/tack_export_phrase.py --port Felucca -o outbox/loop.tack.phrase.json
```

Use the actual verified port name. The JSON export is harmony-only and currently
emits `mode: major`; it must not be interpreted as a complete active-scale or MIDI
clip serialization. `tools/tack_ingest_phrases.py` reads repository `phrases/` and
regenerates compiled USER phrase source; do not run it casually on a reviewed build.
