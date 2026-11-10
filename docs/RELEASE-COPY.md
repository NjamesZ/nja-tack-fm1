# Proposed public project copy — draft, not a Release or a Hub listing

Repository: <https://github.com/NjamesZ/nja-tack-fm1>

The GitHub About text was already “Nja TAK FM-1 Firmware” when this tree was
imported. This import does not change repository settings. A fitting About line,
when the owner chooses to set it:

NJA TACK: free GPL-3.0-only FM-1 firmware for harmony moves and scale-aware MIDI recording, with literal close-onset chords. Felucca-derived. Experimental 6.28-rc5.

## Short introduction

Meet NJA TACK for the M-VAVE FM-1: choose harmony moves on the hardware, play
melodies and chords over MIDI, then decide when to LOOP or APPLY. Close-onset
chords keep their voicing; melody follows the scale. Free software, full source,
upstream credits intact.

## Release / Hub description

6.28-rc5 (FM-1_65262805) is a focused LOOP-screen readability candidate on top of RC3's
recording corrections, with a long identity and app acceptance of both known
flash-chip IDs. Physical compatibility of both chips remains pending. The card
image is a host-rendered 240×240 framebuffer. Internal review logs are not in
this repository. RC3 received positive feedback in one controlled user trial.
On 10 November 2026 at 11:32 HKT one controlled RC5 install succeeded, and an
independent post-reboot handshake reported FM-1_65262805. That does not establish
RC5 audio or the install/recovery matrix. Four recorded voices; 80 ms first-onset
window; literal close-onset pitches; legato follows the scale; new performance
metadata RAM-only; export remains harmony-only.

Repository: https://github.com/NjamesZ/nja-tack-fm1
Exact package: release/6.28-rc5/felucca-6.28-rc5.fwsc
SHA-256: 31067498546ad7ac619987b1499ba61fb95f51ac7a13386bf3a0e3eb8bf5adc3

Before a listing or a GitHub Release: merge and tag, attach the exact package
with matching source and licences, recheck the live identity checker, and finish
the hardware matrix in HUB-READINESS.md. Keep the experimental status visible.
Do not invent play, editor, music, or sheet links. Do not claim an official
partnership or comprehensive audio validation. No Release, tag, or Hub email
has been made.
