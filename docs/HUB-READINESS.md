# Firmware Hub publication checklist — not a listing claim

Sources checked 10 November 2026 HKT:
- https://thefirmwarehub.com/submit/
- https://thefirmwarehub.com/fmhub.md
- https://thefirmwarehub.com/kb/#loader
- https://thefirmwarehub.com/tools/rescue/

## Prepared locally

- Public repository for review: <https://github.com/NjamesZ/nja-tack-fm1>.
  This import is a pull request. It is not merged to the default branch, not
  tagged, and not a GitHub Release. The Hub reads `fmhub.json` from the default
  branch only, so this pull request does not list the project.
- GPL-3.0-only root LICENSE, populated engine sources and component licence texts.
- Root fmhub.json: plain factual description, no advertising. NJA project story
  stays in README. The only path in the file is the relative image
  `docs/images/loop-rc5.png` (240×240 PNG, under 300 KB). `play`, `editor`, and
  `cheat` are omitted: no NJA browser, editor, or sheet URL was supplied, and
  none is invented here. The Hub schema rejects any field outside its list, so
  the repository URL is not stored in `fmhub.json`.
- RC5 running identity FM-1_65262805: format is FM-1_ plus 3–8 digits; 13 characters
  fits the package’s 20-character marker and protocol’s 27-byte identity payload.
  The submit checker’s exact-product and non-generic anchored-range checks against
  the live 172-entry catalogue found no owner/matching reserved range. This does
  not reserve the prefix or guarantee future availability; recheck before publishing.
- RC3/FM-1_991 and RC4/FM-1_992 overlap Dinghy’s FM-1_99[0-8] range. They remain
  local historical builds only. RC3’s user trial, and RC5’s single controlled
  install below, are not the Hub hardware matrix.
- Both app chip IDs accepted: Puya P25Q80 0x856014 and UC25HQ80 0xB36014. Actual
  boot-gate host test covers both and rejects unknown IDs. Existing loader performs
  plain NOR operations and has no JEDEC whitelist; no claim of physical write tests.
- The package embeds the existing Felucca update loader (`FELUCCA-LOADER-1` is
  present). Its 20-byte marker decodes to running identity FM-1_65262805.
  `firmware/loader/loader.c` still defines `ota-FM-1_900`. The app image also
  contains the pre-existing update-entry string `ota-FM-1_015` from
  `firmware/src/ota.c`. Neither loader string was edited for RC5, and neither
  replaces FM-1_65262805 as the running-app identity.

## One RC5 hardware fact, recorded separately from the matrix

- [x] 10 November 2026, 11:32 HKT: one controlled install of this exact package.
      Independent post-reboot handshake reported running identity FM-1_65262805.
      This box is only that event. It does not tick any item below.

## Pending — every item is a release blocker, not a checked test

- [ ] Merge of the browsable tree to the default branch, then a public git tag
      whose tree still has root LICENSE and populated `firmware/engines/`.
      A source attachment alone is insufficient. This pull request is not that tag.
- [ ] GitHub Release with the exact checked `.fwsc` attached and complete matching
      source. Prerelease notes and checksums are in `release/6.28-rc5/`. No Release
      has been created.
- [ ] Root fmhub.json on the default branch; online “Check your file” after that push.
- [ ] Recheck identity availability against the live public checker at publication time.
- [ ] Install the exact release file from official V15 on a real FM-1; boot and identity.
- [ ] Install the same file from another custom firmware; boot and Hub Connect naming.
- [ ] Leave RC5 by installing another firmware using its update loader; verify next identity.
- [ ] Verify USB MIDI enumeration/port (normally Felucca), and FM-1_65262805 response
      on that enumerated port. The 11:32 handshake is not this USB-matrix item.
- [ ] Separately approved controlled interrupted-Writing/recovery/resume test with backup;
      read the Hub rescue instructions first. No unplug/recovery test was done here.
- [ ] Real saves/updates on both flash-chip variants, or clear supported-hardware evidence.
- [ ] RC5 audio listening test, plus hardware timing and voice-budget measurements.
- [ ] Inherited host MIDI/UI/preset failures, drum-grid stress, and the sanitizer
      timeout investigated. DeepSeek correction review was blocked on credentials
      and is not a pass.
- [ ] Separately approved musical bench checklist in INSTALL.md on the exact file.

README already records project/sample incompatibility as unestablished, and
records the RAM-only caveat. Do not tick the matrix using emulator output,
packet counts, RC3 praise, the single RC5 install, or a new build.

## Submission only after readiness

First listing, once section 1 of the Hub instructions is actually complete, is
an email to zven@thefirmwarehub.com with project/maker name, public repository,
unique identity, licence, and truthful completed hardware tests. That email is
not authorized by this import, and none was sent. Do not claim the install/leave
matrix from the single 11:32 install. Later GitHub releases may be ingested
automatically, including prereleases; creating one is a separate decision.

VIBE conversion is a separate request, not this listing path. No plugin/browser
conversion compatibility has been established for TACK. Never imply otherwise.
