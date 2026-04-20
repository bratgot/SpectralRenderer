#!/usr/bin/env python3
"""
apply_sss_label_cleanup.py -- remove "(CPU)" and "GPU planned" labels
from SpectralSurface SSS references. GPU random walk was ported
2026-04-20 and matches CPU pixel-for-pixel, so these labels are stale.

Matches ROADMAP parked item: "Remove '(CPU)' suffix from SSS knob".

Three edits in SpectralSurfaceOp.cpp:
  1. Group title "Subsurface scattering (CPU)" -> "Subsurface scattering"
  2. Info text "<b>Subsurface scattering (CPU, GPU planned)</b>" ->
     "<b>Subsurface scattering (GPU + CPU)</b>"
  3. Footer "SSS on CPU" -> "SSS on GPU + CPU"
"""

import argparse
import sys
from pathlib import Path


EDITS = [
    ("SSS group title: drop (CPU) suffix",
     'BeginClosedGroup(f, "sss_grp", "Subsurface scattering (CPU)");',
     'BeginClosedGroup(f, "sss_grp", "Subsurface scattering");',
     'BeginClosedGroup(f, "sss_grp", "Subsurface scattering");'),
    ("SSS info-text heading: CPU+GPU planned -> GPU + CPU",
     '"<b>Subsurface scattering (CPU, GPU planned)</b><br>"',
     '"<b>Subsurface scattering (GPU + CPU)</b><br>"',
     '"<b>Subsurface scattering (GPU + CPU)</b><br>"'),
    ("Footer: SSS on CPU -> SSS on GPU + CPU",
     'SSS on CPU<br>',
     'SSS on GPU + CPU<br>',
     'SSS on GPU + CPU<br>'),
]


class R:
    APPLIED = "applied"; ALREADY = "already applied"
    NOT_FOUND = "NOT FOUND"; AMBIGUOUS = "AMBIGUOUS"


def apply_edit(text, o, n, marker):
    if marker in text: return text, R.ALREADY
    c = text.count(o)
    if c == 0: return text, R.NOT_FOUND
    if c > 1:  return text, R.AMBIGUOUS
    return text.replace(o, n, 1), R.APPLIED


def process(path, edits, dry, force, bak):
    print(f"\n=== {path.name} ===")
    if not path.exists():
        print(f"  ERROR: missing: {path}"); return False
    with open(path, "rb") as f: raw = f.read()
    crlf = raw.count(b"\r\n"); lf = raw.count(b"\n") - crlf
    uses_crlf = crlf > lf
    print(f"  ({'CRLF' if uses_crlf else 'LF'} line endings)")
    original = raw.decode("utf-8").replace("\r\n", "\n")
    text = original; ok = True
    for desc, a, b, marker in edits:
        text, s = apply_edit(text, a, b, marker)
        mk = {R.APPLIED:"[+]", R.ALREADY:"[=]", R.NOT_FOUND:"[!]", R.AMBIGUOUS:"[?]"}[s]
        print(f"  {mk} {desc}: {s}")
        if s in (R.NOT_FOUND, R.AMBIGUOUS): ok = False

    if text == original:
        print("  (no changes needed)"); return ok
    if not ok and not force:
        print("  SKIPPED WRITE"); return False
    if dry:
        print(f"  DRY RUN: {len(text)-len(original):+d} chars"); return ok
    ob = text.encode(); obk = original.encode()
    if uses_crlf:
        ob = ob.replace(b"\n", b"\r\n"); obk = obk.replace(b"\n", b"\r\n")
    bakp = path.with_suffix(path.suffix + bak)
    bakp.write_bytes(obk); path.write_bytes(ob)
    print(f"  wrote {path.name}; backup {bakp.name}")
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", type=Path, default=Path("src"))
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()
    if not args.src.is_dir():
        print(f"ERROR: --src not found: {args.src}", file=sys.stderr); sys.exit(1)
    bak = ".bak_ssslabel"
    ok = process(args.src / "SpectralSurfaceOp.cpp", EDITS, args.dry_run, args.force, bak)
    if not ok: sys.exit(1)


if __name__ == "__main__":
    main()
