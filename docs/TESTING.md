# Test status and limitations

## RC5 UI review batch

Ten host targets compiled and ran exit 0: `tack_app_rec_test` (32 top-level
assertions plus 4,620 generated/literal pattern combinations and all 128-note
nearest-scale properties), `tack_loop_chord_test` (7 actual audio-block assertions),
`tack_ui_review` (4 UI assertions/15 frames), `test_tack_rec_policy` (53),
`test_tack_keep_and_rec` (28), `audio_test`, `chord_test`, `drum_test`, `persistence_test`, and the actual
`persist_boot` flash-ID gate regression (four IDs).
This repository does not include internal review logs or private diffs.
These are host simulations with real app/DSP paths, not device performance measurements.

The screenshot uses actual `tack_app_draw()` output, real fonts and RGB565 framebuffer
conversion. Its cropped region is y=22…239 of 240×240. No controls/audio implementation
changed from RC3; only LOOP drawing and the boot version string changed in tack_app.c.
The separate persistence gate now accepts Puya P25Q80 and UC25HQ80, rejecting
unknown IDs. The regression failed for UC25HQ80 before the correction and passed
after it. It exits at accepted-ID initialization, before hardware-only sample XIP;
it is not an end-to-end flash persistence hardware test. An initial fixture that
continued into unmapped XIP crashed, so that result was not counted as a pass.

## Exact-file update simulations (not device tests)

Long-ID OTA handshake/staging passed against the exact RC5 package. The real
loader-core host simulation passed RC3→RC5 and RC5→RC3 package writes. Installer
CLI tests passed against a simulated device, including JS/Python package parsing.
None opened a live MIDI device, installed hardware, interrupted physical writing,
or satisfied the Hub’s real-device install/leave requirement.

## Inherited unresolved evidence

The complete upstream host suite is **not green** and was not rerun open-endedly
for this focused UI pass. Earlier full-suite work stopped at drum-grid stress.
In RC3, `midi_control_test`, `v156_test`, and `ui_test` matched isolated 6.27 baseline
failure lists: six MIDI CC/routing assertions, a drum-preset-list expectation and
a category-count assertion. The baseline needed an inert external `tack_midi_cc`
link shim because the host TU omits the app; candidate hooks were likewise unset.
That comparison is not proof of TACK hardware CC routing. It is carried forward,
not falsely described as a fresh RC5 full-suite pass.

RC3 full/reduced ASan/UBSan app attempts timed out at 60 s without diagnostics.
They are unresolved, not passes and not a memory-safety/concurrency proof.
No new sanitizer claim is made. DeepSeek correction review remains **blocked**:
no sanctioned usable harness credentials; no pass was fabricated.

## Hardware status

RC3 (FM-1_991) was installed in one controlled trial; a post-reboot query confirmed
running identity, and the user reported “Love it”. This is subjective positive
trial feedback, not exhaustive timing/audio validation.

RC5 (FM-1_65262805), this exact package: one controlled install succeeded on
10 November 2026 at 11:32 HKT. An independent post-reboot handshake reported
the running identity FM-1_65262805. That is the whole hardware record for RC5.
It does not establish RC5 audio, hardware timing, IRQ or voice-stealing
behaviour, or the exact-file install and recovery matrix. Still pending:

- listening test of this exact file
- timing and voice-budget measurements on hardware
- install from official V15, and from another custom firmware
- leave RC5 by installing another firmware, then check the next identity
- USB MIDI enumeration and the FM-1_65262805 handshake on that port
- controlled interruption during Writing, with a recovery plan read first

No guarantees are made. Inherited baseline-matched failures in
`midi_control_test`, `v156_test`, and `ui_test`, the drum-grid stress stop,
and the sanitizer timeout remain unresolved. The DeepSeek correction pass
stayed blocked: no sanctioned usable harness credentials were available, and
no pass was recorded. None of the above is a full-suite pass, Firmware Hub
acceptance, or VIBE compatibility.

Before a final release: independent source/license review; the RC5 checks
listed above; investigate the inherited host failures, drum stress and
sanitizer timeout; verify a clean environment build against the published
checksums. RAM-only metadata and token-only export remain explicit product limits.
