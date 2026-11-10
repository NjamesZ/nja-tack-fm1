#!/usr/bin/env python3
"""Pull an armed LOOP phrase from FM-1 (SysEx 82) into .tack.phrase.json.

Reverse of tools/tack_ingest_phrases.py / docs/CONSTRAINT-TACK-BRIDGE.md.

Device side: LOOP → EXPORT (arms snapshot). Then:
  python3 tools/tack_export_phrase.py [--port NAME] [-o out.tack.phrase.json]

Without a live device, --decode HEX dumps a packed payload for tests.
"""
from __future__ import annotations
import argparse, json, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUTBOX = ROOT / "outbox"

STYLES = [
    "CLASSIC","JAZZ","JUNGLE","REGGAE","AMBIENT","DUB","BREAKS",
    "BOSSA","WALTZ","FUNK","BEATLES",
]
RHYTHM = ["EVEN","WHOLE","HALF","QTR","SYNC","OFF","SPARSE"]
NOTE = "C C# D D# E F F# G G# A A# B".split()
QUAL = ["maj","min","dim","aug","maj7","min7","dom7","halfdim","sus","sus2"]

ED_HDR = bytes([0x7D, 0x46, 0x4C])
ED_PHRASE_EXPORT = 82


def pack7(data: bytes) -> bytes:
    out = bytearray(); acc = 0; bits = 0
    for b in data:
        acc |= (b & 0xFF) << bits; bits += 8
        while bits >= 7:
            out.append(acc & 0x7F); acc >>= 7; bits -= 7
    if bits:
        out.append(acc & 0x7F)
    return bytes(out)


def unpack7(s: bytes) -> bytes:
    out = bytearray(); acc = 0; bits = 0
    for b in s:
        acc |= (b & 0x7F) << bits; bits += 7
        while bits >= 8:
            out.append(acc & 0xFF); acc >>= 8; bits -= 8
    return bytes(out)


def decode_payload(p: bytes) -> dict:
    if len(p) < 6:
        raise SystemExit(f"short payload ({len(p)})")
    tempo = p[0] | (p[1] << 7)
    style_i = p[2]
    rhy = p[3] % 7
    root = p[4] % 12
    nch = p[5]
    chords = []
    o = 6
    for i in range(nch):
        if o + 2 > len(p):
            break
        r, q = p[o], p[o + 1]; o += 2
        chords.append({
            "degree": int(r % 12),
            "quality": QUAL[q] if q < len(QUAL) else "maj",
            "bars": 1,
            "root": NOTE[r % 12],
        })
    return {
        "format": "tack-phrase/1",
        "name": "LOOP-export",
        "tempo": int(tempo),
        "root": NOTE[root],
        "mode": "major",
        "styleHint": STYLES[style_i] if style_i < len(STYLES) else "CLASSIC",
        "rhythm": RHYTHM[rhy],
        "progressionId": "loop-export",
        "chords": chords,
    }


def sysex_req() -> bytes:
    return b"\xF0" + ED_HDR + bytes([ED_PHRASE_EXPORT]) + b"\xF7"


def pull_device(port: str | None) -> dict:
    try:
        import mido
    except ImportError as e:
        raise SystemExit("need mido (+ python-rtmidi) for live pull") from e
    names = mido.get_input_names()
    outs = mido.get_output_names()
    if not names or not outs:
        raise SystemExit("no MIDI ports")
    inn = port or next((n for n in names if "FM-1" in n or "Felucca" in n), names[0])
    outn = next((n for n in outs if inn.split(":")[0] in n or "FM-1" in n), outs[0])
    with mido.open_input(inn) as mi, mido.open_output(outn) as mo:
        mo.send(mido.Message.from_bytes(sysex_req()))
        t0 = time.time()
        while time.time() - t0 < 2.0:
            for msg in mi.iter_pending():
                if msg.type != "sysex":
                    continue
                raw = bytes(msg.data)
                if len(raw) < 4 or raw[0:3] != ED_HDR or raw[3] != ED_PHRASE_EXPORT:
                    continue
                body = raw[4:]
                # editor replies with 7-bit bytes already in the SysEx data path
                return decode_payload(body)
            time.sleep(0.02)
    raise SystemExit("no phrase reply (arm EXPORT on device first)")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", default=None)
    ap.add_argument("-o", "--out", type=Path, default=None)
    ap.add_argument("--decode", default=None, help="hex payload (no device)")
    args = ap.parse_args()
    if args.decode:
        data = decode_payload(bytes.fromhex(args.decode.replace(" ", "")))
    else:
        data = pull_device(args.port)
    out = args.out
    if out is None:
        OUTBOX.mkdir(parents=True, exist_ok=True)
        out = OUTBOX / "loop-export.tack.phrase.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(data, indent=2) + "\n")
    print(f"wrote {out}")
    print(json.dumps(data, indent=2))


if __name__ == "__main__":
    main()
