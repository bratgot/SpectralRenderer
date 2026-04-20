#!/usr/bin/env python3
"""
apply_housekeeping_roadmap_only.py -- drop two ROADMAP entries that
are already fixed. The subdiv knob-range item is also already fixed
on disk (SetRange is 0-4 and tooltip is corrected), so no src change
needed.

Entries removed:
  1. "Remove '(CPU)' suffix from SSS knob on SpectralSurface"
     (resolved 2026-04-20 via apply_sss_label_cleanup)
  2. "Port SpectralVolumeMaterial preset menu to SpectralSurface"
     (resolved 2026-04-20 via apply_surface_preset_unify +
      apply_surface_preset_fix)

Can also drop the subdiv-level-clamping entry since it's not actually
open anymore.
"""

import argparse
import sys
from pathlib import Path


ROADMAP_SSS_OLD = """- **Remove "(CPU)" suffix from SSS knob on SpectralSurface** (new,
  2026-04-20). The knob was labelled "(CPU)" back when GPU SSS was a
  wrap-diffuse approximation rather than true subsurface scattering.
  After the random-walk port on 2026-04-20, GPU SSS now matches CPU
  pixel-for-pixel (16-step spectral random walk with BVH exit detection,
  same Gaussian basis albedo, wavelength-dependent mean free path).
  Audit SpectralSurfaceOp.cpp / SpectralSurfaceOp.h for any label,
  tooltip, or help string mentioning SSS being CPU-only and remove it.
"""

ROADMAP_PRESET_OLD = """- **Port SpectralVolumeMaterial preset menu to SpectralSurface** (new,
  2026-04-20). User reports the SpectralVolumeMaterial preset dropdown
  works perfectly -- one selection, all category knobs update, mutual
  reset of the other categories, tooltips clear. SpectralSurface's
  preset UI (the split-group polish from earlier this session) works
  but is different in mechanics. Take the VolumeMaterial approach
  across. Probably means: one master enum dropdown replacing the
  eight category dropdowns, driving _ApplyPresetV2; simplifies the UI
  and gives users a single entry point. Keep the existing eight
  category enums as "advanced" if backward-compatibility matters,
  otherwise replace them wholesale. Triage starts by reading
  SpectralVolumeMaterial.cpp's knobs() + knob_changed() to see the
  exact mechanism.
"""

ROADMAP_SUBDIV_OLD = """- **Subdivision level clamping**: knob range is 0-6 but OpenSubdiv clamps
  internally to `[1, 4]`. Tooltip implies 6 works. Either raise the internal
  cap or lower the knob max.
"""


class R:
    APPLIED = "applied"; ALREADY = "already applied"
    NOT_FOUND = "NOT FOUND"; AMBIGUOUS = "AMBIGUOUS"


def apply_remove(text, o):
    if o not in text:
        return text, R.ALREADY
    c = text.count(o)
    if c > 1:
        return text, R.AMBIGUOUS
    return text.replace(o, "", 1), R.APPLIED


EDITS = [
    ("Drop SSS (CPU) suffix entry (resolved 2026-04-20)", ROADMAP_SSS_OLD),
    ("Drop VolumeMaterial->Surface preset port entry (resolved 2026-04-20)", ROADMAP_PRESET_OLD),
    ("Drop subdiv knob-range entry (already fixed on disk)", ROADMAP_SUBDIV_OLD),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--roadmap", type=Path, default=Path("ROADMAP.txt"))
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()
    if not args.roadmap.exists():
        print(f"ERROR: --roadmap not found: {args.roadmap}", file=sys.stderr); sys.exit(1)

    path = args.roadmap
    with open(path, "rb") as f: raw = f.read()
    crlf = raw.count(b"\r\n"); lf = raw.count(b"\n") - crlf
    uses_crlf = crlf > lf
    print(f"\n=== {path.name} ===")
    print(f"  ({'CRLF' if uses_crlf else 'LF'} line endings)")
    original = raw.decode("utf-8").replace("\r\n", "\n")
    text = original; ok = True

    for desc, block in EDITS:
        text, s = apply_remove(text, block)
        mk = {R.APPLIED:"[+]", R.ALREADY:"[=]", R.AMBIGUOUS:"[?]"}[s]
        print(f"  {mk} {desc}: {s}")
        if s == R.AMBIGUOUS: ok = False

    if text == original:
        print("  (no changes needed)"); sys.exit(0 if ok else 1)
    if not ok and not args.force:
        print("  SKIPPED WRITE"); sys.exit(1)
    if args.dry_run:
        print(f"  DRY RUN: {len(text)-len(original):+d} chars"); sys.exit(0 if ok else 1)
    ob = text.encode(); obk = original.encode()
    if uses_crlf:
        ob = ob.replace(b"\n", b"\r\n"); obk = obk.replace(b"\n", b"\r\n")
    bakp = path.with_suffix(path.suffix + ".bak_housekeep")
    bakp.write_bytes(obk); path.write_bytes(ob)
    print(f"  wrote {path.name}; backup {bakp.name}")


if __name__ == "__main__":
    main()
