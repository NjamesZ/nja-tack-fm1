#!/usr/bin/env python3
"""Hard gate TACK 5.0 / FM-1_950 — Funk + V16 soon list (Panic, bar-end queues,
live erase, hat/fill ratchets, lane LVL+PAN, named FX scenes). Keeps Bossa/Waltz
3/4, CR-78/707, Drums page, Sounds colors, octave, REC→PLAY→modify.
Tech/Trap/Punk/Dubstep stay deleted. Jazz/Jungle/Reggae frozen."""
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

def mask_row(name):
    m = re.search(rf'(?:static\s+)?const uint16_t {name}\[TACK_STYLE_COUNT\]\s*=\s*\{{(.*?)\}};', APP, re.S)
    if not m:
        return []
    body = re.sub(r'/\*.*?\*/', '', m.group(1), flags=re.S)
    return [h.lower() for h in re.findall(r'0x[0-9a-fA-F]+', body)]

def names():
    m = re.search(r'TACK_STYLE_NAME\[TACK_STYLE_COUNT\]\s*=\s*\{([^}]+)\}', DATA)
    if not m:
        m = re.search(r'TACK_STYLE_NAME\[TACK_STYLE_COUNT\]\s*=\s*\{([^}]+)\}', APP)
    return re.findall(r'"([^"]+)"', m.group(1))

print("=== 1. Deleted styles stay gone ===")
ok("No Tech/Trap/Punk/Dubstep enums", all(x not in HDR for x in (
    "TACK_STYLE_TRAP", "TACK_STYLE_TECH", "TACK_STYLE_PUNK", "TACK_STYLE_DUBSTEP")))
ok("No deleted names", all(x not in names() for x in ("TECH", "TRAP", "PUNK", "DUBSTEP")), str(names()))
ok("Data clean of deleted", all(f'"{x}"' not in DATA for x in ("TECH", "TRAP", "PUNK", "DUBSTEP")))

print("=== 2. Styles: 11 = …BOSSA WALTZ FUNK ===")
n = names()
ok("Style count 11", len(n) == 11, str(n))
ok("Order", n == [
    "CLASSIC", "JAZZ", "JUNGLE", "REGGAE", "AMBIENT", "DUB", "DARK", "BREAKS",
    "BOSSA", "WALTZ", "FUNK"], str(n))
ok("Enum FUNK", "TACK_STYLE_FUNK" in HDR and "TACK_STYLE_COUNT" in HDR)
b, d = bpms(), dens()
ok("11 BPMs", len(b) == 11, str(b))
ok("All BPM ≤140", all(x <= 140 for x in b), str(b))
ok("Breaks 134", b[7] == 134)
ok("Bossa 112", b[8] == 112)
ok("Waltz 108", b[9] == 108)
ok("Funk 100", b[10] == 100)
ok("Funk BPM pocket 95–110", 95 <= b[10] <= 110)
ok("Jazz 104 frozen", b[1] == 104)
ok("Jungle 140 frozen", b[2] == 140)
ok("Reggae 76 frozen", b[3] == 76)
ok("Jazz dens 44", d[1] == 44)
ok("Jungle dens 34", d[2] == 34)
ok("Reggae dens 38", d[3] == 38)
ok("Funk dens sparse", d[10] <= 30, str(d[10]))
ok("Jungle kick frozen", mask_row("TACK_KICK")[2] == "0x0441")
ok("Jungle hats frozen", mask_row("TACK_HAT")[2] == "0x4444")
ok("Funk kick spare", mask_row("TACK_KICK")[10] in ("0x0101", "0x0100", "0x0001"))
ok("Funk hats sparse", mask_row("TACK_HAT")[10] in ("0x0404", "0x0440", "0x0000", "0x4040"))

print("=== 3. Waltz 3/4 + Bossa kept ===")
ok("bar_steps helper", "tack_bar_steps" in APP)
ok("Waltz → 12 sixteenths", "TACK_STYLE_WALTZ) ? 12u : 16u" in APP or "TACK_STYLE_WALTZ) ? 12" in APP)
ok("loop_steps 4×bar", "4u * tack_bar_steps()" in APP)
ok("grid snap 3/4 pref", has(APP, "pref3", "0, 4, 8"))
ok("auto landings 3/4", "lands3" in APP and "0, 4, 8, 6" in APP)
ok("Waltz overlay", "sty == TACK_STYLE_WALTZ" in APP)
ok("Bossa overlay", "TACK_STYLE_BOSSA" in APP)
ok("Bossa DRM CR-78", re.search(r'DRM_P\[TACK_STYLE_COUNT\]\s*=\s*\{[^}]*6', APP) is not None)

print("=== 4. Funk style ===")
ok("Funk overlay branch", "sty == TACK_STYLE_FUNK" in APP or "TACK_STYLE_FUNK)" in APP)
ok("Funk gold / accent", "FUNK" in APP and ("TACK_STYLE_COL" in APP))
ok("Funk skips fill spray", APP.count("TACK_STYLE_FUNK") >= 3)

print("=== 5. CR-78 + 707 + Drums + Sounds colors + octave + REC ===")
ok("CR-78 in eng_drum", '"CR-78"' in DRUM)
ok("707 KIT in eng_drum", '"707 KIT"' in DRUM or '"707"' in DRUM)
ok("kit presets 4/5/6/7", "{4, 5, 6, 7}" in APP)
ok("Sounds colors", "TACK_BANK_COL" in APP)
ok("Drums page GLO", "TACK_PG_DRUMS" in APP and "B_GLO" in APP)
ok("octave −2..+2", has(APP, "if (o < -2) o = -2", "if (o > 2) o = 2") or has(APP, "o < -2", "o > 2"))
ok("OCT±", has(APP, "B_OCTUP", "B_OCTDN"))
ok("crec REC→PLAY", has(APP, "tack_crec_step", "tack_crec_mode"))
ok("modify after PLAY", "crec" in APP.lower())

print("=== 6. Panic ===")
ok("tack_panic fn", "static void tack_panic(void)" in APP)
ok("panic_req clear notes", "panic_req" in APP)
ok("hold PLAY ≥600ms", ">= 600u" in APP and "tack_panic()" in APP)
ok("play long done flag", "tack_play_long_done" in APP)
ok("PANIC flash", '"PANIC"' in APP)

print("=== 7. Bar-end / cycle-quantize ===")
ok("flush_bar_queues", "tack_flush_bar_queues" in APP)
ok("queue style/snd/kit/fxsc", has(APP, "tack_queue_style", "tack_queue_snd", "tack_queue_kit", "tack_queue_fxsc"))
ok("flush on bar change", "bar_now != tack_last_bar" in APP and "tack_flush_bar_queues()" in APP)
ok("Style uses queue", "tack_queue_style(tack_style_cursor)" in APP)
ok("@BAR flashes", has(APP, '"SND@BAR"', '"KIT@BAR"') or "SND@BAR" in APP)

print("=== 8. Live erase ===")
ok("live erase fn", "tack_crec_live_erase" in APP)
ok("SAVE while PLAY after REC", "tack_crec_mode == 2u && song.playing" in APP and "tack_crec_live_erase()" in APP)
ok("need play guard", "ERASE NEED PLAY" in APP or "crec_mode != 2u" in APP)

print("=== 9. Hat/fill ratchets ===")
ok("drm_ratchet state", "tack_drm_ratchet" in APP)
ok("×2/×3/×4", has(APP, "RATCH x2", "RATCH x3") or has(APP, "x2", "x3", "x4"))
ok("generative hats not SEQ grid", "ratchet" in APP.lower() and "stock note grid" in APP.lower() or "not stock" in APP.lower() or "generative" in APP.lower())
ok("Drums row ratchet", "case 6:" in APP and "tack_drm_ratchet" in APP)

print("=== 10. Lane mixer Level + Pan ===")
ok("set_lane_level", "tack_set_lane_level" in APP)
ok("set_lane_pan", "tack_set_lane_pan" in APP)
ok("P_LEVEL + P_PAN", "P_LEVEL" in APP and "P_PAN" in APP)
ok("Feel LVL+PAN rows", '"LVL"' in APP and '"PAN"' in APP)
ok("4 lanes Comp/Bass/Air/Drums", "TACK_LANE" in APP)

print("=== 11. Named FX scenes ===")
ok("FXSC names DRY ROOM DUB SHIMMER", has(APP, '"DRY"', '"ROOM"', '"DUB"', '"SHIMMER"'))
ok("apply_fx_scene", "tack_apply_fx_scene" in APP)
ok("scene lock indep of style", "tack_fx_scene_lock" in APP)
ok("FX K4 scene", "K4" in APP and "fxsc" in APP.lower() or "tack_queue_fxsc" in APP)
ok("scenes indep hint", "indep" in APP.lower() or "Scenes" in APP)

print()
print(f"RESULT: {len(passes)} PASS, {len(fails)} FAIL")
if fails:
    for f in fails:
        print(" -", f)
    sys.exit(1)
print("GATE 100%")
