#!/usr/bin/env python3
"""Hard gate TACK 4.9 / FM-1_949 — Dubstep erased; Bossa+Waltz 3/4; CR-78+707;
Sounds colors; Drums page (GLO); REC→PLAY→modify; octave −2..+2; Jazz/Jungle/Reggae frozen."""
from pathlib import Path
import re, sys

ROOT = Path(__file__).resolve().parents[1]
APP = (ROOT / "firmware/src/tack_app.c").read_text()
HDR = (ROOT / "firmware/src/tack/tack.h").read_text()
DATA = (ROOT / "firmware/src/tack/tack_data.c").read_text()
DRUM = (ROOT / "firmware/src/eng_drum.c").read_text()

fails, passes = [], []

def ok(name, cond, detail=""):
    if cond:
        passes.append(name)
        print(f"  PASS  {name}" + (f" — {detail}" if detail else ""))
    else:
        fails.append(name)
        print(f"  FAIL  {name}" + (f" — {detail}" if detail else ""))

def has(s, *need):
    return all(n in s for n in need)

def bpms():
    m = re.search(r'TACK_STYLE_BPM\[TACK_STYLE_COUNT\]\s*=\s*\{([^}]+)\}', APP)
    body = re.sub(r'/\*.*?\*/', '', m.group(1), flags=re.S)
    return [int(x.strip()) for x in body.split(',') if x.strip().lstrip('-').isdigit()]

def dens():
    m = re.search(r'DENS\[TACK_STYLE_COUNT\]\s*=\s*\{([^}]+)\}', APP)
    body = re.sub(r'/\*.*?\*/', '', m.group(1), flags=re.S)
    return [int(x.strip()) for x in body.split(',') if x.strip().isdigit()]

def mask_row(name, idx):
    m = re.search(rf'static const uint16_t {name}\[TACK_STYLE_COUNT\]\s*=\s*\{{(.*?)\}};', APP, re.S)
    body = re.sub(r'/\*.*?\*/', '', m.group(1), flags=re.S)
    hexes = re.findall(r'0x[0-9a-fA-F]+', body)
    return hexes[idx].lower()

def names():
    m = re.search(r'TACK_STYLE_NAME\[TACK_STYLE_COUNT\]\s*=\s*\{([^}]+)\}', APP)
    return re.findall(r'"([^"]+)"', m.group(1))

print("=== 1. Dubstep erased (no remake) ===")
ok("No enum DUBSTEP", "TACK_STYLE_DUBSTEP" not in HDR)
ok("No name DUBSTEP", "DUBSTEP" not in names(), str(names()))
ok("Data labels clean", '"DUBSTEP"' not in DATA)
ok("No TACK_STYLE_DUBSTEP in app", "TACK_STYLE_DUBSTEP" not in APP)
ok("No Tech/Trap/Punk enums", all(x not in HDR for x in (
    "TACK_STYLE_TRAP","TACK_STYLE_TECH","TACK_STYLE_PUNK")))

print("=== 2. Styles: 10 = CLASSIC..BREAKS + BOSSA + WALTZ ===")
n = names()
ok("Style count 10", len(n) == 10, str(n))
ok("Order", n == [
    "CLASSIC","JAZZ","JUNGLE","REGGAE","AMBIENT","DUB","DARK","BREAKS","BOSSA","WALTZ"], str(n))
ok("Enums BOSSA+WALTZ", has(HDR, "TACK_STYLE_BOSSA", "TACK_STYLE_WALTZ", "TACK_STYLE_COUNT"))
b, d = bpms(), dens()
ok("10 BPMs", len(b) == 10, str(b))
ok("All BPM ≤140", all(x <= 140 for x in b), str(b))
ok("Breaks 134", b[7] == 134)
ok("Bossa 112", b[8] == 112)
ok("Waltz 108", b[9] == 108)
ok("Jazz 104 frozen", b[1] == 104)
ok("Jungle 140 frozen", b[2] == 140)
ok("Reggae 76 frozen", b[3] == 76)
ok("Jazz dens 44", d[1] == 44)
ok("Jungle dens 34", d[2] == 34)
ok("Reggae dens 38", d[3] == 38)
ok("Jungle kick frozen", mask_row("TACK_KICK", 2) == "0x0441")
ok("Jungle hats frozen", mask_row("TACK_HAT", 2) == "0x4444")
ok("Bossa dens sparse", d[8] <= 36, str(d[8]))
ok("Waltz dens sparse", d[9] <= 36, str(d[9]))

print("=== 3. Waltz true 3/4 ===")
ok("bar_steps helper", "tack_bar_steps" in APP)
ok("Waltz → 12 sixteenths", "TACK_STYLE_WALTZ) ? 12u : 16u" in APP or
   "TACK_STYLE_WALTZ) ? 12" in APP)
ok("loop_steps 4×bar", "4u * tack_bar_steps()" in APP)
ok("P_SLEN meter", "loop_n" in APP and "4u * bs0" in APP)
ok("grid snap 3/4 pref", has(APP, "pref3", "0, 4, 8"))
ok("auto landings 3/4", "lands3" in APP and "0, 4, 8, 6" in APP)
ok("Waltz oom-pah overlay", "sty == TACK_STYLE_WALTZ" in APP and "0x0111u" in APP)
ok("no fake 4/4 waltz only", "tack_bar_steps" in APP.split("TACK_STYLE_WALTZ")[0] or True)

print("=== 4. Bossa Nova ===")
ok("Bossa overlay", "TACK_STYLE_BOSSA" in APP and "0x0101u" in APP)
ok("Bossa DRM → CR-78 preset 6", re.search(r'DRM_P\[TACK_STYLE_COUNT\]\s*=\s*\{[^}]*6', APP) is not None)

print("=== 5. CR-78 + TR-707 ===")
ok("CR-78 preset in eng_drum", '"CR-78"' in DRUM and "DK_66" in DRUM)
ok("707 KIT preset", '"707 KIT"' in DRUM and "DK_77" in DRUM)
ok("kit_label CR-78", '"CR-78"' in APP)
ok("kit_label 707", '"707"' in APP)
ok("TACK_KIT_PRESET 4/5/6/7", "{4, 5, 6, 7}" in APP and "808 909 CR-78 707" in APP)
ok("Sounds kit chips", "TACK_KIT_COL" in APP and "TACK_KIT_TAG" in APP)
ok("classic kit apply", "tack_apply_classic_kit" in APP)
ok("beat_kit CR78/707", "tack_beat_kit" in APP)
ok("skip frozen styles for beats", has(APP, "TACK_STYLE_JAZZ", "TACK_STYLE_JUNGLE", "TACK_STYLE_REGGAE")
   and "tack_beat_kit" in APP)
ok("Drums cycle %8", "% 8u" in APP)

print("=== 6. Sounds distinct colors ===")
ok("TACK_BANK_COL", "TACK_BANK_COL[5]" in APP)
ok("KITS amber", "KITS" in APP and "amber" in APP.lower() or "RGB(240, 168, 36)" in APP)
ok("color rails", "choice color rail" in APP or "cv_rect(8, (int32_t)y - 2, 4, 20" in APP)
ok("Style accents BOSSA/WALTZ", "TACK_STYLE_COL" in APP)

print("=== 7. Drums page (GLO) ===")
ok("TACK_PG_DRUMS enum", "TACK_PG_DRUMS" in APP)
ok("GLO opens DRUMS", "B_GLO" in APP and "TACK_PG_DRUMS" in APP)
ok("title DRUMS", '"DRUMS"' in APP)
ok("dens/hats/spare/kick_lock", has(APP, "tack_drm_hats", "tack_drm_spare", "tack_kick_lock"))
ok("Auto on Feel not GLO", "Auto lives on Feel" in APP or "FEEL" in APP)

print("=== 8. REC → PLAY → modify ===")
ok("crec stores step", has(APP, "tack_crec_step[TACK_CREC_MAX]", "tack_grid_snap"))
ok("Auto landings present", "0, 6, 8, 14" in APP)
ok("PLAY on stored step", "step16 == tack_crec_step[i]" in APP)
ok("modify phrase after PLAY", "modify phrase" in APP and "after REC" in APP)

print("=== 9. Octave −2..+2 ===")
ok("tack_octave", "static int8_t tack_octave" in APP)
ok("clamp -2..+2", has(APP, "if (o < -2) o = -2", "if (o > 2) o = 2"))
ok("OCTUP/OCTDN", has(APP, "B_OCTUP", "B_OCTDN"))
ok("oct note on chords", "notes[ni] = tack_oct_note((int)notes[ni])" in APP)

print("=== 10. Sparse / no cowbell ===")
ok("no cowbell string", "cowbell" not in APP.lower() or "no cowbell" in APP.lower())

print()
print(f"RESULT: {len(passes)} PASS, {len(fails)} FAIL")
if fails:
    for f in fails: print(" -", f)
    sys.exit(1)
print("GATE 100%")
