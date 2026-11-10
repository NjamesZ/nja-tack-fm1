# Source provenance and distribution

NJA TACK is a local Felucca-derived development snapshot, not a clean upstream tag.
Upstream project: <https://github.com/hugelton/Felucca> by Leo Kuroshita / Hügelton
Instruments. See CREDITS.md and LICENSING.md for retained third-party attribution.
NJA branding does not claim authorship of upstream engines, assets or firmware.

RC5 was copied from the isolated RC4 UI review, itself copied from frozen RC3; the working/live tree, RC3 and historical 6.27
were not edited. The RC5 app diff is confined to LOOP drawing and its boot version
label. RC4 (FM-1_992) remains unchanged as a superseded local-only build.
The build map adds FM-1_65262805 for RC5; host renderer/tests and clean documentation
are added. Python companion defaults and two legacy test paths become repo-relative
for portability/privacy. No audio/recording/control feature was introduced. A separate minimal app
persistence JEDEC gate correction accepts both known 1 MiB chip IDs; the unchanged
loader already has no old-chip-only filter. Physical writes on both variants
remain untested for this exact candidate.

The source archive contains firmware, populated MIT engines, assets, fonts,
samples with attribution, tools, web source and host tests. It excludes private
working copies, .git data, credentials, caches, logs, SDK/toolchain installations,
old patch scratch and stale historical guides. Source files are checksummed in
SOURCE-MANIFEST.sha256. Generated build data is recreated by the documented tools;
compiled USER phrases matching the reviewed candidate remain in source.

The actual `.fwsc` contains three unmodified Apache-2.0 JieLi SDK files as described
in LICENSING.md. The SDK/toolchain are prerequisites rather than copied private
installations. Keep the full source archive and licence/attribution files next to
the matching firmware whenever redistributing. Generic forge ZIP archives may
omit engines; inspect before offering one as corresponding source.

The sealed archive `release/6.28-rc5/nja-tack-6.28-rc5-source.tar.xz`
(SHA-256 `dc288b502ed801ab981e64791b860a4757c0616464a34e0a5f9abcbe2c898713`,
4642208 bytes) is that handoff tree, byte for byte. This repository expands it
at the root so engines, licences, docs, images, and guides are browsable.
`SOURCE-MANIFEST.sha256` matches the expanded publication tree after the
documentation pass below. The archive still carries the pre-adaptation manifest
inside it.

Documentation adapted after the archive was sealed, without editing firmware
C, headers, assembly, engine sources, or the package binaries:

- README, CHANGELOG, CANDIDATE-IDENTITY, CONTRIBUTING, BUILDING, `.gitignore`
- docs: INSTALL, TESTING, QUICKSTART, RECORDING, HUB-READINESS, RELEASE-COPY,
  SOURCE-PROVENANCE
- added root `SHA256SUMS` and `release/6.28-rc5/` (exact `.fwsc`, app image,
  sealed source archive, checksums, prerelease notes)

The adaptations point links at <https://github.com/NjamesZ/nja-tack-fm1>,
record the 10 November 2026 11:32 HKT install, and keep relative doc links.
`fmhub.json` was left unchanged: its image path was already the relative
`docs/images/loop-rc5.png`, and the Hub schema has no repository field.

No secret, private path, unpublished credential, internal review log, or
private diff is part of this tree. A git tag, a GitHub Release, and a Firmware
Hub listing are still future steps. The import envelope zip was verified
(SHA-256 `7c01a15e332296742702f46701b802f389143e68d8888a98672d75d367292eaa`)
and is not republished; its docs are the pre-adaptation copies.
