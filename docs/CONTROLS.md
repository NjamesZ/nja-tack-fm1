# Control guide — 6.28-rc5

K1–K4 mean the four rotary controls, not piano keys 1–6. Turning a knob is an action;
page-local roles differ. This guide describes TACK, not stock FM-1 firmware.

## Navigation and HOME

| Control | Normal action / caveat |
| --- | --- |
| HOME | Return from edit page; short on HOME advances focused lane; hold 450 ms opens arranger editing |
| EDIT | LIBRARY; inside library choose sounds, not a mode/kit change |
| SCL | STYLE; K1 browse, K2 or SELECT/SEQ apply (may queue while playing) |
| LFO | TRACK SND |
| FX | FX ROOM |
| GLO | DRUMS PART → drum FEEL → HOME; navigation does not remake patterns |
| ENV short / hold 450 ms | GEN / FEEL respectively |
| REC from normal pages | Open LOOP; on FEEL it has the existing HOLD role |
| OCT− / OCT+ | Generated harmonic octave −2…+2; both reset; HOME long OCT+ 500 ms toggles show LOCK |
| Keys 1–6 on HOME/LOOP | Harmony moves, including during REC; they are not raw MIDI pitches |
| K1 / K2 on normal HOME | Browse / commit a move |
| K3 / K4 on normal HOME | Switch phrase A/B / change mode |
| ARP short on HOME | DRUMS PART; hold 450 ms creates BRIDGE (destructive harmony action) |

MORE, setlist and arranger-edit contexts change HOME knob roles. SAVE held browses
setlist with K1; a long SAVE can write a performance. These legacy saves do not
persist new literal MIDI performance metadata. Avoid destructive controls until
comfortable; use the on-screen page/context and LOCK state.

## LOOP

K1 chooses one of five rows; K2 performs its action. K4 APPLY requires a held take;
when empty it can load a compiled USER phrase instead. Debounce applies, so avoid
rapid repeated turns. Locked destructive actions show LOCKED.

| Row | K2 | K3 |
| --- | --- | --- |
| REC | Arm / stop to HELD | Browse phrase cursor when nonempty |
| LOOP | Explicit playback on/off | Browse existing cursor |
| QUANTIZE | Toggle existing harmony quantize after stop | Browse style/rebind |
| RHYTHM | Cycle EVEN/WHOLE/HALF/QTR/SYNC/OFF/SPARSE after stop | Browse rhythm |
| EXPORT | Arm legacy harmony snapshot (READY) | No new performance-export function |

Short physical REC on LOOP performs the REC cycle. Hold REC ≥600 ms clears the
capture and requests release of LOOP voices. SELECT/SEQ returns HOME here rather
than committing a harmony move. SAVE during transport playback with a held take
can live-erase; it is not a lossless performance backup.

RC5 shows four legible phrase chips at a time, following the existing cursor.
The header shows cursor/total. This display change does not alter the take.

## Sound editing and drums

LIBRARY: K1 browses libraries; K2/SELECT applies and opens that library's presets.
Within presets K1 selects the focused track's sound, K2 applies to the band, K3
changes focused track, K4 returns to libraries. SAVE opens PRESETS.
DRUMS PART: K1 step, K2 hit/erase, K3 lane, K4 remake; remake is destructive.
Other legacy FX/FEEL/GEN pages retain RC3 behavior and page labels. This UI pass
is not a redesign or exhaustive revalidation of those pages.

## Played MIDI is separate

Incoming USB/TRS MIDI note-on/off messages carry actual pitch/channel identity;
keys 1–6 carry move choices. Keep both paths in mind when combining moves and
played notes. MIDI CC routing has known host-test caveats; see [TESTING.md](TESTING.md).
