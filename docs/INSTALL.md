# Install, verify and recover

**One controlled install of this exact RC5 package succeeded on 10 November 2026
at 11:32 HKT.** After reboot, an independent handshake reported running identity
FM-1_65262805. That single event is not an audio test, not a timing or
voice-budget measurement, and not the exact-file matrix (official V15, another
custom firmware, leave-again, USB enumeration, or a controlled interruption).
Build success is not permission to install and not proof of safe audio. Any
further install is a separate decision. Firmware installation can fail and is
at your own risk; no warranty is provided.

## Before installation

1. Confirm this is an M-VAVE FM-1, not another MIDI device. Back up projects/presets
   with a compatible existing workflow; RAM-only recordings are not recoverable
   from legacy project backups.
2. Keep your known-good package and its checksum on disk before proceeding.
   The developer's RC3 and historical 6.27 recovery copies remain preserved;
   this source archive does not silently bundle or relabel those binaries.
3. Compare the candidate's SHA256SUMS. RC5 identity is **FM-1_65262805 / 6.28-rc5**;
   RC3 is FM-1_991. Do not substitute a similarly named older file.
4. Close other MIDI editors/installers/DAWs using the port. Use stable USB power,
   low audio volume and a direct verified cable. Don't let the computer sleep
   during the bounded installation. Do not repeatedly retry a protocol mismatch.

## Documented terminal installer

Prerequisites: Python 3, `mido`, `python-rtmidi`; see `tools/fm1_install.py --help`.
From the full source directory:

```sh
python3 tools/fm1_install.py --info
# Review the detected device and actual MIDI port. Only after deciding to install
# this exact published package (not a fresh local rebuild):
python3 tools/fm1_install.py --port Felucca release/6.28-rc5/felucca-6.28-rc5.fwsc
python3 tools/fm1_install.py --port Felucca --info
```

`Felucca` is an example port, not a wildcard instruction to install on another device.
The path above is the checked 610090-byte package in this repository. A local
`./build.sh --release 6.28-rc5` writes `build/felucca-6.28-rc5.fwsc` instead.
Treat that file as this candidate only after its SHA-256 matches
`release/6.28-rc5/SHA256SUMS`. There is no GitHub Release asset yet.
The installer handles loader entry, transfer and reboot. Read its result and verify
post-reboot running identity **FM-1_65262805**. Packet transmission alone is not success.
If the port/device/protocol doesn't match, stop and record the exact error.

Do not use an upstream web installer assuming it distributes NJA TACK. This
project's repository is <https://github.com/NjamesZ/nja-tack-fm1>. It has no
GitHub Release and no Hub listing yet. When either exists, it has to point at
this exact package and this full source, including populated `firmware/engines/`.

## Recovery

If the app still starts, use the same documented installer with your verified
known-good package, then query the running identity again. If it no longer starts,
follow the hardware-specific instructions of
[FM-1-transporter](https://github.com/kurogedelic/FM-1-transporter).
Read those instructions before any user-only cable/power/boot step. Do not guess
button sequences or force another device. Official vendor firmware is not included.

See [HUB-READINESS.md](HUB-READINESS.md) for the stricter exact-file install-from-V15,
other-firmware, leave-again and controlled interruption matrix. All remain pending.

## Short bench checklist after a separately approved install

- Confirm version, then low-volume transport start/stop and drums.
- Record separate C–D–E; switch major/minor and root; listen for nearest melody mapping.
- Record overlapping C–E–G within 80 ms; change scale/root/octave; hear the same voicing.
- Play legato notes more than 80 ms apart; ensure they remain scale-following melody.
- Try rolled note releases, LOOP off, long-REC CLEAR and rearm; check no stuck notes.
- Keep keys 1–6 as move choices; combine one move with MIDI notes.
- Test four voices, then intentional overflow/rejection; disconnect MIDI loopback.
- Note clicks, voice stealing, latency, missed notes or redraw issues with exact settings.

Do not call a silent or visually successful check an audio pass.
