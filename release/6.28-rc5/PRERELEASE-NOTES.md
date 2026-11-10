# 6.28-rc5 prerelease notes — not a GitHub Release

Identity: **6.28-rc5 / FM-1_65262805**

Repository: <https://github.com/NjamesZ/nja-tack-fm1>

These notes prepare a future release. They do not create a git tag or a GitHub
Release, and they do not submit the project to The Firmware Hub.

## Exact assets

Preserved from the reviewed build. Not rebuilt, not patched.

| File | Bytes | SHA-256 |
| --- | ---: | --- |
| `felucca-6.28-rc5.fwsc` | 610090 | `31067498546ad7ac619987b1499ba61fb95f51ac7a13386bf3a0e3eb8bf5adc3` |
| `felucca-6.28-rc5-app.bin` | 457268 | `d8dba643ea1004572f45a73f25379f8e496261d15d5b5367d2219fe22b6e6628` |
| `nja-tack-6.28-rc5-source.tar.xz` | 4642208 | `dc288b502ed801ab981e64791b860a4757c0616464a34e0a5f9abcbe2c898713` |

From this directory: `sha256sum -c SHA256SUMS`

The package marker decodes to `FM-1_65262805`. The app image contains
`FELUCCA v6.28-rc5` and `FM-1_65262805`. The sealed `.tar.xz` is the source
tree before documentation links and the 10 November install record were applied
in the browsable repository. Firmware sources inside that archive match the
firmware sources in the repository.

Import envelope (verified, not republished):
`nja-tack-6.28-rc5-review.zip`
SHA-256 `7c01a15e332296742702f46701b802f389143e68d8888a98672d75d367292eaa`.

## What a later GitHub Release should attach

- `felucca-6.28-rc5.fwsc` (required installable package)
- this `SHA256SUMS`
- the matching source: the repository tree at the release tag, with
  `firmware/engines/` populated, or the sealed `nja-tack-6.28-rc5-source.tar.xz`
  plus a note that documentation links were updated after that archive
- `LICENSE`, `LICENSING.md`, and `LICENSES/` from the repository root
- `assets/samples-cc0/ATTRIBUTION.txt`

Licence texts are GPL-3.0-only for the application, with MIT, Apache-2.0, OFL,
and CC0 notices kept for engines and third-party parts. Upstream Felucca remains
credited to Leo Kuroshita / Hügelton Instruments. NJA did not author that upstream.

## Status to keep visible

Experimental candidate.

On 10 November 2026 at 11:32 HKT, one controlled install of this exact package
succeeded. An independent post-reboot handshake reported FM-1_65262805.
RC3 had previously been user-tested, with positive subjective feedback.

Still open, and not implied by that install:

- RC5 audio listening test
- hardware timing and voice-budget measurements
- exact-file install from official V15 and from another custom firmware
- leaving RC5 for another firmware
- USB enumeration check
- controlled interruption / recovery
- inherited host MIDI, UI, and preset failures
- sanitizer timeout
- DeepSeek correction review (blocked: no usable harness credentials)

No full-suite pass, Firmware Hub acceptance, or VIBE compatibility is claimed.

Played notes follow the selected scale. Overlapping notes inside an 80 ms
window anchored at the first onset keep their literal pitches. Legato follows
the scale. Keys 1–6 pick harmony moves. Four concurrent recorded voices maximum;
a larger take is rejected as a whole. New performance metadata is RAM-only.

## Blocked until a later, separate decision

- merge this tree to the default branch
- public git tag
- GitHub Release
- Firmware Hub email to zven@thefirmwarehub.com
- any new install beyond the single recorded RC5 event
