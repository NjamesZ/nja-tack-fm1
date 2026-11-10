# Changelog

## 6.28-rc5 — FM-1_65262805 — pre-publication review candidate

- Carry forward the isolated LOOP readability pass without changing audio/REC controls.
- Replace colliding short release identity with a currently free 8-digit identity,
  checked using the public Hub checker rules and live catalogue.
- App persistence boot now accepts Puya 0x856014 and UC25HQ80 0xB36014; unknown
  IDs remain rejected. Actual boot-gate regression failed before and passed after.
- Existing loader has no JEDEC allowlist rejecting either chip; no loader behavior
  was changed. Host simulation does not establish physical write compatibility.
- Add root fmhub.json, actual square image and explicit Hub hardware test checklist.
- 10 November 2026, 11:32 HKT: one controlled install of this exact package
  succeeded. An independent post-reboot handshake reported FM-1_65262805.
  RC5 audio, timing, voice-budget, and the exact-file V15 / custom / leave /
  USB / interruption matrix remain pending.
- Source, the exact package, and guides are prepared in this repository.
  No git tag, GitHub Release, or Firmware Hub submission.

## 6.28-rc4 — FM-1_992 — superseded local-only UI review

This historical short identity overlaps Dinghy’s declared Hub range and must not
be published as NJA’s own identity. Its original build was preserved unchanged.

- LOOP: align row labels/values; remove collisions between bank/status/rows/footer.
- Use font-supported ASCII labels, four readable cursor-following phrase chips,
  concise knob/MIDI-vs-move guidance and an honest harmony-only EXPORT hint.
- ARMED no longer implies a live finalized-note count.
- Preserve RC3 audio, record, harmony-move controls and 80 ms policy unchanged.
- Add a real firmware framebuffer renderer and bounded UI regressions.
- Add NJA quick start, controls, recording limits, installation/recovery and
  honest test/source/licensing documentation; portable companion/test paths.
- Built and host-tested; not flashed or hardware-qualified.

## 6.28-rc3 — FM-1_991 — controlled hardware trial

- Absolute-nearest scale mapping across octave boundaries with lower-note ties.
- Performed literal voicing preserved through patterned and real LOOP playback.
- Channel/raw-pitch recording ownership, queued main-loop capture and audio-owned
  LOOP scheduling with individual releases and loop span after final note-off.
- Four-voice and bounded-capacity rejection; no pitch-only echo suppression;
  internal playback does not re-record; mixed move/MIDI takes and metadata reset.
- Keep user-owned four-slot chords across generated take/mode/style rebuilds;
  preserve move picks and in-RAM A/B/undo metadata.
- Installed identity verified; user gave positive subjective trial feedback.
  Host failures/sanitizer timeout and RAM-only persistence remain documented.

## Earlier lineage

Derived from local TACK 6.27 and earlier Felucca-based work. Do not interpret the
numbering as upstream Felucca releases. Upstream rights and third-party notices
are preserved; see CREDITS.md and LICENSING.md.
