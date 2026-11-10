#!/usr/bin/env python3
"""Watch FM-1 for armed LOOP EXPORT (SysEx 82) and write timestamped JSON to outbox.

Mac companion for TACK 6.14. Device: LOOP → EXPORT (READY). Then:

  python3 tools/tack_export_watch.py [--port NAME] [--outbox DIR] [--once]

Each successful pull writes:
  ./outbox/tack-phrase-YYYYMMDD-HHMMSS.json
Ctrl-C to stop. --once exits after one successful pull.
"""
from __future__ import annotations
import argparse, json, sys, time
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from tack_export_phrase import OUTBOX, STYLES, pull_device  # type: ignore


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", default=None)
    ap.add_argument("--outbox", type=Path, default=OUTBOX)
    ap.add_argument("--once", action="store_true")
    ap.add_argument("--interval", type=float, default=1.5)
    a = ap.parse_args()
    a.outbox.mkdir(parents=True, exist_ok=True)
    print(f"watching EXPORT → {a.outbox}", flush=True)
    while True:
        try:
            data = pull_device(a.port)
            stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
            out = a.outbox / f"tack-phrase-{stamp}.json"
            out.write_text(json.dumps(data, indent=2) + "\n")
            sty_i = int(data.get("style", 0)) % len(STYLES)
            print(f"EXPORT → {out}  ({STYLES[sty_i]}, {data.get('nch', '?')} chords)", flush=True)
            if a.once:
                return 0
        except SystemExit as e:
            msg = str(e)
            if a.once:
                print(msg or "no phrase", file=sys.stderr)
                return 1
            # idle: EXPORT not armed / no reply yet
        except Exception as e:
            print(f"watch: {e}", file=sys.stderr)
            if a.once:
                return 1
        time.sleep(a.interval)


if __name__ == "__main__":
    raise SystemExit(main())
